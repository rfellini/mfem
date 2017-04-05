// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "../config/config.hpp"

#ifdef MFEM_USE_OCCA

#include "obilinearform.hpp"
#include "obilininteg.hpp"
#include "ointerpolation.hpp"
#include "../linalg/osparsemat.hpp"

#include "tfe.hpp"

namespace mfem {
  //---[ Bilinear Form ]----------------
  OccaBilinearForm::IntegratorBuilderMap OccaBilinearForm::integratorBuilders;

  OccaBilinearForm::OccaBilinearForm(FiniteElementSpace *f) :
    Operator(f->GetVSize()) {
    Init(occa::currentDevice(), f);
  }

  OccaBilinearForm::OccaBilinearForm(occa::device device_, FiniteElementSpace *f) :
    Operator(f->GetVSize()) {
    Init(device, f);
  }

  void OccaBilinearForm::Init(occa::device device_, FiniteElementSpace *f) {
    fes = f;
    mesh = fes->GetMesh();
    device = device_;

    SetupIntegratorBuilderMap();
    SetupKernels();
    SetupIntegratorData();
    SetupInterpolationData();
  }

  void OccaBilinearForm::SetupIntegratorBuilderMap() {
    if (0 < integratorBuilders.size()) {
      return;
    }
    integratorBuilders[DiffusionIntegrator::StaticName()] =
      new OccaDiffusionIntegrator(*this);
  }

  void OccaBilinearForm::SetupKernels() {
    baseKernelProps["defines/NUM_VDIM"] = GetVDim();

    occa::properties mapProps("defines: {"
                              "  TILESIZE: 256,"
                              "}");

    vectorExtractKernel = device.buildKernel("occa://mfem/linalg/mappings.okl",
                                             "VectorExtract",
                                             mapProps);
    vectorAssembleKernel = device.buildKernel("occa://mfem/linalg/mappings.okl",
                                              "VectorAssemble",
                                              mapProps);
  }

  void OccaBilinearForm::SetupIntegratorData() {
    const FiniteElement &fe = GetFE(0);
    const H1_TensorBasisElement *el = dynamic_cast<const H1_TensorBasisElement*>(&fe);

    const Table &e2dTable = fes->GetElementToDofTable();
    const int *elementMap = e2dTable.GetJ();
    const int elements = GetNE();
    const int numDofs = GetNDofs();
    const int localDofs = fe.GetDof();

    const int *dofMap;
    if (el) {
      dofMap = el->GetDofMap().GetData();
    } else {
      int *dofMap_ = new int[localDofs];
      for (int i = 0; i < localDofs; ++i) {
        dofMap_[i] = i;
      }
      dofMap = dofMap_;
    }

    int *offsets = new int[numDofs + 1];
    int *indices = new int[elements * localDofs];

    // We'll be keeping a count of how many local nodes point
    //   to its global dof
    for (int i = 0; i <= numDofs; ++i) {
      offsets[i] = 0;
    }

    for (int e = 0; e < elements; ++e) {
      for (int d = 0; d < localDofs; ++d) {
        const int gid = elementMap[localDofs*e + d];
        ++offsets[gid + 1];
      }
    }
    // Aggregate to find offsets for each global dof
    for (int i = 1; i <= numDofs; ++i) {
      offsets[i] += offsets[i - 1];
    }
    // For each global dof, fill in all local nodes that point
    //   to it
    for (int e = 0; e < elements; ++e) {
      for (int d = 0; d < localDofs; ++d) {
        const int gid = elementMap[localDofs*e + dofMap[d]];
        const int lid = localDofs*e + d;
        indices[offsets[gid]++] = lid;
      }
    }
    // We shifted the offsets vector by 1 by using it
    //   as a counter. Now we shift it back.
    for (int i = numDofs; i > 0; --i) {
      offsets[i] = offsets[i - 1];
    }
    offsets[0] = 0;

    // Allocate device offsets and indices
    globalToLocalOffsets = device.malloc((numDofs + 1) * sizeof(int),
                                         offsets);
    globalToLocalIndices = device.malloc((elements * localDofs) * sizeof(int),
                                         indices);

    delete [] offsets;
    delete [] indices;
    if (!el) {
      delete [] dofMap;
    }

    // Allocate a temporary vector where local element operations
    //   will be handled.
    localX.SetSize(device, elements * localDofs);
  }

  void OccaBilinearForm::SetupInterpolationData() {
    const SparseMatrix *R = fes->GetRestrictionMatrix();
    const Operator *P = fes->GetProlongationMatrix();
    CreateRPOperators(device,
                      R, P,
                      restrictionOp,
                      prolongationOp);
  }

  occa::device OccaBilinearForm::GetDevice() {
    return device;
  }

  int OccaBilinearForm::BaseGeom() const {
    return mesh->GetElementBaseGeometry();
  }

  Mesh& OccaBilinearForm::GetMesh() const {
    return *mesh;
  }

  int OccaBilinearForm::GetDim() const
  { return mesh->Dimension(); }

  int64_t OccaBilinearForm::GetNE() const
  { return mesh->GetNE(); }

  int64_t OccaBilinearForm::GetNDofs() const
  { return fes->GetNDofs(); }

  int64_t OccaBilinearForm::GetVDim() const
  { return fes->GetVDim(); }

  const FiniteElement& OccaBilinearForm::GetFE(const int i) const {
    return *(fes->GetFE(i));
  }

  // Adds new Domain Integrator.
  void OccaBilinearForm::AddDomainIntegrator(BilinearFormIntegrator *bfi,
                                             const occa::properties &props) {
    AddIntegrator(*bfi, props, DomainIntegrator);
  }

  // Adds new Boundary Integrator.
  void OccaBilinearForm::AddBoundaryIntegrator(BilinearFormIntegrator *bfi,
                                               const occa::properties &props) {
    AddIntegrator(*bfi, props, BoundaryIntegrator);
  }

  // Adds new interior Face Integrator.
  void OccaBilinearForm::AddInteriorFaceIntegrator(BilinearFormIntegrator *bfi,
                                                   const occa::properties &props) {
    AddIntegrator(*bfi, props, InteriorFaceIntegrator);
  }

  // Adds new boundary Face Integrator.
  void OccaBilinearForm::AddBoundaryFaceIntegrator(BilinearFormIntegrator *bfi,
                                                   const occa::properties &props) {
    AddIntegrator(*bfi, props, BoundaryFaceIntegrator);
  }

  // Adds Integrator based on OccaIntegratorType
  void OccaBilinearForm::AddIntegrator(BilinearFormIntegrator &bfi,
                                       const occa::properties &props,
                                       const OccaIntegratorType itype) {
    OccaIntegrator *builder = integratorBuilders[bfi.Name()];
    if (builder == NULL) {
      std::stringstream error_ss;
      error_ss << "OccaBilinearForm::";
      switch (itype) {
      case DomainIntegrator      : error_ss << "AddDomainIntegrator";       break;
      case BoundaryIntegrator    : error_ss << "AddBoundaryIntegrator";     break;
      case InteriorFaceIntegrator: error_ss << "AddInteriorFaceIntegrator"; break;
      case BoundaryFaceIntegrator: error_ss << "AddBoundaryFaceIntegrator"; break;
      }
      error_ss << " (...):\n"
               << "  No kernel builder for occa::BilinearFormIntegrator '" << bfi.Name() << "'";
      const std::string error = error_ss.str();
      mfem_error(error.c_str());
    }
    integrators.push_back(builder->CreateInstance(bfi,
                                                  baseKernelProps + props,
                                                  itype));
  }

  // Get the finite element space prolongation matrix
  const Operator* OccaBilinearForm::GetProlongation() const {
    return prolongationOp;
  }

  // Get the finite element space restriction matrix
  const Operator* OccaBilinearForm::GetRestriction() const {
    return restrictionOp;
  }

  // Map the global dofs to local nodes
  void OccaBilinearForm::VectorExtract(const OccaVector &globalVec,
                                       OccaVector &localVec) const {

    vectorExtractKernel((int) GetNDofs(),
                        globalToLocalOffsets,
                        globalToLocalIndices,
                        globalVec, localVec);
  }

  // Aggregate local node values to their respective global dofs
  void OccaBilinearForm::VectorAssemble(const OccaVector &localVec,
                                        OccaVector &globalVec) const {

    vectorAssembleKernel((int) GetNDofs(),
                         globalToLocalOffsets,
                         globalToLocalIndices,
                         localVec, globalVec);
  }

  //
  void OccaBilinearForm::Assemble() {
    // [MISSING] Find geometric information that is needed by intergrators
    //             to share between integrators.
    const int integratorCount = (int) integrators.size();
    for (int i = 0; i < integratorCount; ++i) {
      integrators[i]->Assemble();
    }
  }

  // Matrix vector multiplication.
  void OccaBilinearForm::Mult(const OccaVector &x, OccaVector &y) const {
    VectorExtract(x, localX);

    const int integratorCount = (int) integrators.size();
    for (int i = 0; i < integratorCount; ++i) {
      integrators[i]->Mult(localX);
    }

    VectorAssemble(localX, y);
  }

  // Matrix transpose vector multiplication.
  void OccaBilinearForm::MultTranspose(const OccaVector &x, OccaVector &y) const {
    mfem_error("occa::OccaBilinearForm::MultTranspose() is not overloaded!");
  }

  Operator* OccaBilinearForm::CreateRAPOperator(const Operator &Rt,
                                                Operator &A,
                                                const Operator &P) {

    return new TRAPOperator<OccaVector>(Rt, A, P);
  }


  void OccaBilinearForm::FormLinearSystem(const Array<int> &ess_tdof_list,
                                          OccaVector &x, OccaVector &b,
                                          Operator* &Aout, OccaVector &X, OccaVector &B,
                                          int copy_interior) {

    TFormLinearSystem<OccaVector>(ess_tdof_list,
                                  x, b, Aout, X, B,
                                  copy_interior);
  }


  void OccaBilinearForm::RecoverFEMSolution(const OccaVector &X,
                                            const OccaVector &b,
                                            OccaVector &x) {
    TRecoverFEMSolution<OccaVector>(X, b, x);
  }

  void OccaBilinearForm::ImposeBoundaryConditions(const Array<int> &ess_tdof_list,
                                                  Operator *rap,
                                                  Operator* &Aout,
                                                  OccaVector &X, OccaVector &B) {
    OccaConstrainedOperator *A = new OccaConstrainedOperator(device,
                                                             rap, ess_tdof_list,
                                                             rap != this);

    A->EliminateRHS(X, B);
    Aout = A;
  }

  // Frees memory bilinear form.
  OccaBilinearForm::~OccaBilinearForm() {
    // Free memory
    globalToLocalOffsets.free();
    globalToLocalIndices.free();

    // Free kernels
    vectorExtractKernel.free();
    vectorAssembleKernel.free();

    // Make sure all integrators free their data
    IntegratorVector::iterator it = integrators.begin();
    while (it != integrators.end()) {
      delete *it;
      ++it;
    }
  }
  //====================================

  //---[ Constrained Operator ]---------
  occa::kernelBuilder OccaConstrainedOperator::map_dof_builder =
                  makeCustomBuilder("vector_map_dofs",
                                    "const int idx = v2[i];"
                                    "v0[idx] = v1[idx];",
                                    "defines: { VTYPE2: 'int' }");

  occa::kernelBuilder OccaConstrainedOperator::clear_dof_builder =
                  makeCustomBuilder("vector_clear_dofs",
                                    "v0[v1[i]] = 0.0;",
                                    "defines: { VTYPE1: 'int' }");

  OccaConstrainedOperator::OccaConstrainedOperator(Operator *A_,
                                                   const Array<int> &constraint_list_,
                                                   bool own_A_) :
    Operator(A_->Height(), A_->Width()) {
    setup(occa::currentDevice(), A_, constraint_list_, own_A_);
  }

  OccaConstrainedOperator::OccaConstrainedOperator(occa::device device_,
                                                   Operator *A_,
                                                   const Array<int> &constraint_list_,
                                                   bool own_A_) :
    Operator(A_->Height(), A_->Width()) {
    setup(device_, A_, constraint_list_, own_A_);
  }

  void OccaConstrainedOperator::setup(occa::device device_,
                                      Operator *A_,
                                      const Array<int> &constraint_list_,
                                      bool own_A_) {
    device = device_;

    A = A_;
    own_A = own_A_;

    constraint_indices = constraint_list_.Size();
    if (constraint_indices) {
      constraint_list = device.malloc(constraint_indices * sizeof(int),
                                      constraint_list_.GetData());
    }

    z.SetSize(device, height);
    w.SetSize(device, height);
  }

  void OccaConstrainedOperator::EliminateRHS(const OccaVector &x, OccaVector &b) const {
    if (constraint_indices == 0) {
      return;
    }
    occa::kernel map_dofs = map_dof_builder.build(device);

    w = 0.0;

    map_dofs(constraint_indices, w, x, constraint_list);

    A->Mult(w, z);

    b -= z;

    map_dofs(constraint_indices, b, x, constraint_list);
  }

  void OccaConstrainedOperator::Mult(const OccaVector &x, OccaVector &y) const {
    if (constraint_indices == 0) {
      A->Mult(x, y);
      return;
    }

    occa::kernel map_dofs = map_dof_builder.build(device);
    occa::kernel clear_dofs = clear_dof_builder.build(device);

    z = x;

    clear_dofs(constraint_indices, z, constraint_list);

    A->Mult(z, y);

    map_dofs(constraint_indices, y, x, constraint_list);
  }

  OccaConstrainedOperator::~OccaConstrainedOperator() {
    if (own_A) {
      delete A;
    }
    constraint_list.free();
  }
  //====================================
}

#endif