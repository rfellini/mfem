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

#include "ointerpolation.hpp"

namespace mfem {
  void CreateRPOperators(occa::device device,
                         const SparseMatrix *R, const Operator *P,
                         Operator *&OccaR, Operator *&OccaP) {
    if (!P) {
      OccaR = OccaP = NULL;
      return;
    }

    const SparseMatrix *pmat = dynamic_cast<const SparseMatrix*>(P);

    if (R) {
      OccaSparseMatrix *occaR = CreateMappedSparseMatrix(device, *R);
      occa::memory reorderIndices;
      reorderIndices.swap(occaR->reorderIndices);
      occaR->Free();

      OccaR = new OccaRestrictionOperator(device,
                                          R->Height(), R->Width(),
                                          reorderIndices);
    }

    if (pmat) {
      const SparseMatrix *pmatT = Transpose(*pmat);

      OccaSparseMatrix *occaP  = CreateMappedSparseMatrix(device, *pmat);
      OccaSparseMatrix *occaPT = CreateMappedSparseMatrix(device, *pmatT);

      OccaP = new OccaProlongationOperator(*occaP, *occaPT);
    } else {
      OccaP = new OccaProlongationOperator(P);
    }
  }

  OccaRestrictionOperator::OccaRestrictionOperator(occa::device device,
                                                   const int height_, const int width_,
                                                   occa::memory indices) :
    Operator(height_, width_) {

    entries     = indices.entries<int>() / 2;
    trueIndices = indices;

    mapKernel = device.buildKernel("occa://mfem/linalg/mappings.okl",
                                   "ExtractSubVector",
                                   "defines: { TILESIZE: 256 }");
  }

  void OccaRestrictionOperator::Mult(const OccaVector &x, OccaVector &y) const {
    mapKernel(entries, trueIndices, x, y);
  }

  OccaProlongationOperator::OccaProlongationOperator(OccaSparseMatrix &multOp_,
                                                     OccaSparseMatrix &multTransposeOp_) :
    Operator(multOp_.Height(), multOp_.Width()),
    pmat(NULL),
    multOp(multOp_),
    multTransposeOp(multTransposeOp_) {}

  OccaProlongationOperator::OccaProlongationOperator(const Operator *pmat_) :
    Operator(pmat_->Height(), pmat_->Width()),
    pmat(pmat_),
    hostX(pmat_->Width()),
    hostY(pmat_->Height()) {}

  void OccaProlongationOperator::Mult(const OccaVector &x, OccaVector &y) const {
    if (pmat) {
      MPI_Barrier(MPI_COMM_WORLD);
      x.GetData().copyTo(hostX.GetData());
      pmat->Mult(hostX, hostY);
      y.GetData().copyFrom(hostY.GetData());
    } else {
      multOp.Mult(x, y);
    }
  }

  void OccaProlongationOperator::MultTranspose(const OccaVector &x, OccaVector &y) const {
    if (pmat) {
      x.GetData().copyTo(hostY.GetData());
      pmat->MultTranspose(hostY, hostX);
      y.GetData().copyFrom(hostX.GetData());
    } else {
      multTransposeOp.Mult(x, y);
    }
  }
}

#endif
