// Copyright (c) 2010-2020, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "tmop.hpp"
#include "tmop_pa.hpp"
#include "linearform.hpp"
#include "../general/forall.hpp"
#include "../linalg/kernels.hpp"

namespace mfem
{

MFEM_REGISTER_TMOP_KERNELS(void, AddMultPA_Kernel_C0_3D,
                           const double lim_normal,
                           const Vector &lim_dist,
                           const Vector &c0_,
                           const int NE,
                           const DenseTensor &j_,
                           const Array<double> &w_,
                           const Array<double> &b_,
                           const Vector &x0_,
                           const Vector &x1_,
                           Vector &y_,
                           const int d1d,
                           const int q1d)
{
   const bool const_c0 = c0_.Size() == 1;

   constexpr int DIM = 3;
   const int D1D = T_D1D ? T_D1D : d1d;
   const int Q1D = T_Q1D ? T_Q1D : q1d;

   const auto C0 = const_c0 ?
                   Reshape(c0_.Read(), 1, 1, 1, 1) :
                   Reshape(c0_.Read(), Q1D, Q1D, Q1D, NE);
   const auto LD = Reshape(lim_dist.Read(), D1D, D1D, D1D, 1, NE);
   const auto J = Reshape(j_.Read(), DIM, DIM, Q1D, Q1D, Q1D, NE);
   const auto b = Reshape(b_.Read(), Q1D, D1D);
   const auto W = Reshape(w_.Read(), Q1D, Q1D, Q1D);
   const auto X0 = Reshape(x0_.Read(), D1D, D1D, D1D, DIM, NE);
   const auto X1 = Reshape(x1_.Read(), D1D, D1D, D1D, DIM, NE);

   auto Y = Reshape(y_.ReadWrite(), D1D, D1D, D1D, DIM, NE);

   MFEM_FORALL_3D(e, NE, Q1D, Q1D, Q1D,
   {
      const int D1D = T_D1D ? T_D1D : d1d;
      const int Q1D = T_Q1D ? T_Q1D : q1d;
      constexpr int MQ1 = T_Q1D ? T_Q1D : T_MAX;
      constexpr int MD1 = T_D1D ? T_D1D : T_MAX;

      MFEM_SHARED double B[MQ1*MD1];

      MFEM_SHARED double DDD[3][MD1*MD1*MD1];
      MFEM_SHARED double DDQ[3][MD1*MD1*MQ1];
      MFEM_SHARED double DQQ[3][MD1*MQ1*MQ1];
      MFEM_SHARED double QQQ[3][MQ1*MQ1*MQ1];

      MFEM_SHARED double DDD0[3][MD1*MD1*MD1];
      MFEM_SHARED double DDQ0[3][MD1*MD1*MQ1];
      MFEM_SHARED double DQQ0[3][MD1*MQ1*MQ1];
      MFEM_SHARED double QQQ0[3][MQ1*MQ1*MQ1];

      MFEM_SHARED double DDD1[3][MD1*MD1*MD1];
      MFEM_SHARED double DDQ1[3][MD1*MD1*MQ1];
      MFEM_SHARED double DQQ1[3][MD1*MQ1*MQ1];
      MFEM_SHARED double QQQ1[3][MQ1*MQ1*MQ1];

      kernels::LoadX<MD1>(e,D1D,0,LD,DDD[0]);
      kernels::LoadX<MD1>(e,D1D,X0,DDD0);
      kernels::LoadX<MD1>(e,D1D,X1,DDD1);

      kernels::LoadB<MD1,MQ1>(D1D,Q1D,b,B);

      kernels::EvalX<MD1,MQ1>(D1D,Q1D,B,DDD,DDQ);
      kernels::EvalY<MD1,MQ1>(D1D,Q1D,B,DDQ,DQQ);
      kernels::EvalZ<MD1,MQ1>(D1D,Q1D,B,DQQ,QQQ);

      kernels::EvalX<MD1,MQ1>(D1D,Q1D,B,DDD0,DDQ0);
      kernels::EvalY<MD1,MQ1>(D1D,Q1D,B,DDQ0,DQQ0);
      kernels::EvalZ<MD1,MQ1>(D1D,Q1D,B,DQQ0,QQQ0);

      kernels::EvalX<MD1,MQ1>(D1D,Q1D,B,DDD1,DDQ1);
      kernels::EvalY<MD1,MQ1>(D1D,Q1D,B,DDQ1,DQQ1);
      kernels::EvalZ<MD1,MQ1>(D1D,Q1D,B,DQQ1,QQQ1);

      MFEM_FOREACH_THREAD(qz,z,Q1D)
      {
         MFEM_FOREACH_THREAD(qy,y,Q1D)
         {
            MFEM_FOREACH_THREAD(qx,x,Q1D)
            {
               const double *Jtr = &J(0,0,qx,qy,qz,e);
               const double detJtr = kernels::Det<3>(Jtr);
               const double weight = W(qx,qy,qz) * detJtr;

               double D[3], p0[3], p1[3];
               const double coeff0 = const_c0 ? C0(0,0,0,0) : C0(qx,qy,qz,e);
               kernels::PullEval<MQ1>(qx,qy,qz,QQQ,D);
               kernels::PullEval<MQ1>(qx,qy,qz,QQQ0,p0);
               kernels::PullEval<MQ1>(qx,qy,qz,QQQ1,p1);

               double d1[3];
               // Eval_d1
               // subtract(1.0 / (dist * dist), x, x0, d1);
               // z = a * (x - y)
               // grad = a * (x - x0)
               const double dist = D[0]; // GetValues, default comp set to 0
               const double a = 1.0 / (dist * dist);
               const double w = weight * lim_normal * coeff0;
               kernels::Subtract<3>(w*a, p1, p0, d1);
               kernels::PushEval<MQ1>(qx,qy,qz,d1,QQQ0);
            }
         }
      }
      MFEM_SYNC_THREAD;
      kernels::LoadBt<MD1,MQ1>(D1D,Q1D,b,B);
      kernels::EvalXt<MD1,MQ1>(D1D,Q1D,B,QQQ0,DQQ0);
      kernels::EvalYt<MD1,MQ1>(D1D,Q1D,B,DQQ0,DDQ0);
      kernels::EvalZt<MD1,MQ1>(D1D,Q1D,B,DDQ0,Y,e);
   });
}

void TMOP_Integrator::AddMultPA_C0_3D(const Vector &X, Vector &Y) const
{
   const int N = PA.ne;
   const int D1D = PA.maps->ndof;
   const int Q1D = PA.maps->nqpt;
   const int id = (D1D << 4 ) | Q1D;
   const double ln = lim_normal;
   const Vector &LD = PA.LD;
   const DenseTensor &J = PA.Jtr;
   const Array<double> &W = IntRule->GetWeights();
   const Array<double> &B = PA.maps->B;
   const Vector &X0 = PA.X0;
   const Vector &C0 = PA.C0;

   MFEM_LAUNCH_TMOP_KERNEL(AddMultPA_Kernel_C0_3D,id,ln,LD,C0,N,J,W,B,X0,X,Y);
}

} // namespace mfem