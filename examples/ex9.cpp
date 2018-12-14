//                                MFEM Example 9
//
// Compile with: make ex9
//
// Sample runs:
//    ex9 -m ../data/periodic-segment.mesh -p 0 -r 2 -dt 0.005
//    ex9 -m ../data/periodic-square.mesh -p 0 -r 2 -dt 0.01 -tf 10
//    ex9 -m ../data/periodic-hexagon.mesh -p 0 -r 2 -dt 0.01 -tf 10
//    ex9 -m ../data/periodic-square.mesh -p 1 -r 2 -dt 0.005 -tf 9
//    ex9 -m ../data/periodic-hexagon.mesh -p 1 -r 2 -dt 0.005 -tf 9
//    ex9 -m ../data/amr-quad.mesh -p 1 -r 2 -dt 0.002 -tf 9
//    ex9 -m ../data/star-q3.mesh -p 1 -r 2 -dt 0.005 -tf 9
//    ex9 -m ../data/disc-nurbs.mesh -p 1 -r 3 -dt 0.005 -tf 9
//    ex9 -m ../data/disc-nurbs.mesh -p 2 -r 3 -dt 0.005 -tf 9
//    ex9 -m ../data/periodic-square.mesh -p 3 -r 4 -dt 0.0025 -tf 9 -vs 20
//    ex9 -m ../data/periodic-cube.mesh -p 0 -r 2 -o 2 -dt 0.02 -tf 8
//    ex9 -m ../data/periodic-square.mesh -p 4 -r 4 -o 0 -dt 0.01 -tf 4 -s 1 -mt 0
//    ex9 -m ../data/periodic-square.mesh -p 4 -r 4 -o 1 -dt 0.001 -tf 4 -s 1 -mt 0
//    ex9 -m ../data/periodic-square.mesh -p 4 -r 4 -o 1 -dt 0.002 -tf 4 -s 2 -mt 1
//    ex9 -m ../data/periodic-square.mesh -p 4 -r 4 -o 1 -dt 0.0008 -tf 4 -s 3 -mt 2 -st 1
//
// Description:  This example code solves the time-dependent advection equation
//               du/dt + v.grad(u) = 0, where v is a given fluid velocity, and
//               u0(x)=u(0,x) is a given initial condition.
//
//               The example demonstrates the use of Discontinuous Galerkin (DG)
//               bilinear forms in MFEM (face integrators), the use of explicit
//               ODE time integrators, the definition of periodic boundary
//               conditions through periodic meshes, as well as the use of GLVis
//               for persistent visualization of a time-evolving solution. The
//               saving of time-dependent data files for external visualization
//               with VisIt (visit.llnl.gov) is also illustrated.

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>

using namespace std;
using namespace mfem;

// Choice for the problem setup. The fluid velocity, initial condition and
// inflow boundary condition are chosen based on this parameter.
int problem;

// Velocity coefficient
void velocity_function(const Vector &x, Vector &v);

// Initial condition
double u0_function(const Vector &x);

// Inflow boundary condition
double inflow_function(const Vector &x);

// Mesh bounding box
Vector bb_min, bb_max;

enum MONOTYPE { None, DiscUpw, DiscUpw_FS, Rusanov, Rusanov_FS, ResDist, ResDist_FS, ResDist_Lim, ResDist_LimMass };
enum STENCIL  { Full, Local, LocalAndDiag };


class SolutionBounds
{

   // set of local dofs which are in stencil of given local dof
   Mesh* mesh;
   FiniteElementSpace* fes;

   STENCIL stencil;

   // metadata for computing local bounds

   // Info for all dofs, including ones on face-neighbor cells.
   mutable DenseMatrix DOFs_coord;                   // size #dofs

public:

   // Map to compute localized bounds on unstructured grids.
   // For each dof index we have a vector of neighbor dof indices.
   mutable std::map<int, std::vector<int> > map_for_bounds;

   Vector x_min;
   Vector x_max;

   SolutionBounds(FiniteElementSpace* _fes, const BilinearForm& K,
                  STENCIL _stencil)
   {
      fes = _fes;
      mesh = fes->GetMesh();
      stencil = _stencil;

      if (stencil > 0) { GetBoundsMap(fes, K); }
   }

   void Compute(const SparseMatrix &K, const Vector &x)
   {
      x_min.SetSize(x.Size());
      x_max.SetSize(x.Size());

      switch (stencil)
      {
         case 0:
            ComputeFromSparsity(K, x);
            break;
         case 1:
         case 2:
            ComputeLocalBounds(x);
            break;
         default:
            mfem_error("Unsupported stencil.");
      }
   }

   void ComputeFromSparsity(const SparseMatrix& K, const Vector& x)
   {
      const int *I = K.GetI(), *J = K.GetJ(), size = K.Size();

      for (int i = 0, k = 0; i < size; i++)
      {
         double x_i_min = numeric_limits<double>::infinity();
         double x_i_max = -x_i_min;
         for (int end = I[i+1]; k < end; k++)
         {
            double x_j = x(J[k]);

            if (x_j > x_i_max)
            {
               x_i_max = x_j;
            }
            if (x_j < x_i_min)
            {
               x_i_min = x_j;
            }
         }
         x_min(i) = x_i_min;
         x_max(i) = x_i_max;
      }
   }

   // Computation of localized bounds.
   void ComputeLocalBounds(const Vector &x)
   {
      const int size = x.Size();
      //const Vector &x_nd = x.FaceNbrData(); // for parallel

      for (int i = 0; i < size; i++)
      {
         double x_i_min = +numeric_limits<double>::infinity();
         double x_i_max = -x_i_min;
         for (int j = 0; j < (int)map_for_bounds[i].size(); j++)
         {
            const int dof_id = map_for_bounds[i][j];
            double x_j = x(map_for_bounds[i][j]);
            // const double x_j = (dof_id < size) ? x(map_for_bounds[i][j])
            //                                : x_nd(dof_id - size); // for parallel
            if (x_j > x_i_max) { x_i_max = x_j; }
            if (x_j < x_i_min) { x_i_min = x_j; }
         }
         x_min(i) = x_i_min;
         x_max(i) = x_i_max;
      }
   }

private:

   double distance_(const IntegrationPoint &a, const IntegrationPoint &b)
   {
      return sqrt((a.x - b.x) * (a.x - b.x) +
                  (a.y - b.y) * (a.y - b.y) +
                  (a.z - b.z) * (a.z - b.z));
   }

   double Distance(const int dof1, const int dof2) const
   {
      const int dim = fes->GetMesh()->Dimension();

      if (dim==1)
      {
         return abs(DOFs_coord(0, dof1) - DOFs_coord(0, dof2));
      }
      else if (dim==2)
      {
         const double d1 = DOFs_coord(0, dof1) - DOFs_coord(0, dof2),
                      d2 = DOFs_coord(1, dof1) - DOFs_coord(1, dof2);
         return sqrt(d1*d1 + d2*d2);
      }
      else
      {
         const double d1 = DOFs_coord(0, dof1) - DOFs_coord(0, dof2),
                      d2 = DOFs_coord(1, dof1) - DOFs_coord(1, dof2),
                      d3 = DOFs_coord(2, dof1) - DOFs_coord(2, dof2);
         return sqrt(d1*d1 + d2*d2 + d3*d3);
      }
   }

   // Fills DOFs_coord
   void ComputeCoordinates(FiniteElementSpace *fes)
   {
      const int dim = fes->GetMesh()->Dimension();
      const int num_cells = fes->GetNE();
      const int NDOFs     = fes->GetVSize();
      DOFs_coord.SetSize(dim, NDOFs);
      // DOFs_coord.SetSize(dim, NDOFs + fes->num_face_nbr_dofs); // for parallel

      Array<int> ldofs;
      DenseMatrix physical_coord;

      // Cells for the current process.
      for (int i = 0; i < num_cells; i++)
      {
         const IntegrationRule &ir = fes->GetFE(i)->GetNodes();
         ElementTransformation *el_trans = fes->GetElementTransformation(i);

         el_trans->Transform(ir, physical_coord);
         fes->GetElementDofs(i, ldofs);

         for (int j = 0; j < ldofs.Size(); j++)
         {
            for (int d = 0; d < dim; d++)
            {
               DOFs_coord(d, ldofs[j]) = physical_coord(d, j);
            }
         }
      }

      // Face-neighbor cells.
      /* for parallel
      IsoparametricTransformation el_trans;
      for (int i = 0; i < fes->GetMesh()->face_nbr_elements.Size(); i++)
      {
         const IntegrationRule &ir = fes->GetFaceNbrFE(i)->GetNodes();
         fes->GetMesh()->GetFaceNbrElementTransformation(i, &el_trans);

         el_trans.Transform(ir, physical_coord);
         fes->GetFaceNbrElementVDofs(i, ldofs);

         for (int j = 0; j < ldofs.Size(); j++)
         {
            ldofs[j] += NDOFs;

            for (int d = 0; d < dim; ++d)
            {
               DOFs_coord(d, ldofs[j]) = physical_coord(d, j);
            }
         }
      } */
   }

   // Fills map_for_bounds
   void GetBoundsMap(FiniteElementSpace *fes, const BilinearForm &K)
   {
      ComputeCoordinates(fes);

      int num_cells = fes->GetMesh()->GetNE();
      int NDOFs     = fes->GetVSize();
      double dist_level, dist = 0;
      const double tol = 1.e-10;
      Array<int> ldofs;
      Array<int> ldofs_external;
      const int *I = K.SpMat().GetI(), *J = K.SpMat().GetJ();

      // use the first mesh element as indicator
      switch (stencil)
      {
         case 1:
            // hk at ref element with some tolerance
            dist_level = 1.0 / fes->GetOrder(0) + tol;
            break;
         case 2:
            // Include the diagonal neighbors, use the first mesh element as indicator
            // modified by Hennes, this should be larger than sqrt(3) to support 3D
            dist_level = 1.8 / fes->GetOrder(0) + tol;
            break;
         default:
            mfem_error("Unsupported stencil.");
      }

      // what is the sense of this? I replaced boundsmap with map_for_bounds
      //std::map< int, std::vector<int> > &boundsmap = F.init_state.map_for_bounds;

      const FiniteElement *fe_external;

      for (int k = 0; k < num_cells; k++)
      {
         fes->GetElementDofs(k, ldofs);
         const FiniteElement &fe = *fes->GetFE(k);
         int n_dofs = fe.GetDof();
         const IntegrationRule &ir = fe.GetNodes();

         // Use for debugging.
#define DOF_ID -1

         // Fill map_for_bounds for each dof within the cell.
         for (int i = 0; i < n_dofs; i++)
         {
            //////////////////////
            // ADD INTERNAL DOF //
            //////////////////////
            // For the cell where ith-DOF lives look for DOFs within dist(1).
            // This distance has to be on the reference element
            for (int j = 0; j < n_dofs; j++)
            {
               if (distance_(ir.IntPoint(i), ir.IntPoint(j)) <= dist_level)
               {
                  map_for_bounds[ldofs[i]].push_back(ldofs[j]);
               }
            }
            if (ldofs[i] == DOF_ID)
            {
               for (int j = 0; j < (int)map_for_bounds[DOF_ID].size(); j++)
               {
                  cout << map_for_bounds[DOF_ID][j] << endl;
               }
            }

            //////////////////////
            // ADD EXTERNAL DOF //
            //////////////////////
            // There are different sources of external DOF.
            // 1. If one of the already (internal) included DOFs for the
            //    ith position is at a "face" then I have to include all external
            //    DOFs at the face location.
            // 2. If the ith-DOF is at a "face", then I have to include external
            //    DOFs within distance from the i-th location.
            // 3. Periodic BC - points that are at the boundary of the domain or a
            //    DOF next to it have to consider DOFs on the other end of the
            //    domain (NOT IMPLEMENTED YET!!!).

            //////////////
            // SOURCE 1 //
            //////////////
            // Loop over the already included internal DOFs (except the ith-DOF).
            // For each, find if its sparsity pattern contains
            // other DOFs with same physical location, and add them to the map.
            /*
             *         vector<int> vector_of_internal_dofs = map_for_bounds[ldofs[i]];
             *         for (int it = 0; it < vector_of_internal_dofs.size(); it++)
             *         {
             *            const int idof = vector_of_internal_dofs[it];
             *            if (idof == ldofs[i]) { continue; }
             *
             *            // check sparsity pattern
             *            for (int j = I[idof]; j < I[idof + 1]; j++)
             *            {
             *               if (idof != J[j] && Distance(idof, J[j]) <= tol)
             *               {
             *                  boundsmap[ldofs[i]].push_back(J[j]);
            }
            }
            }

            if (ldofs[i] == DOF_ID)
            {
            cout << "sdf " << vector_of_internal_dofs.size() << endl;
            for (int j = 0; j < F.init_state.map_for_bounds[DOF_ID].size(); j++)
            {
            cout << boundsmap[DOF_ID][j] << endl;
            }
            }
            */

            //////////////
            // SOURCE 2 //
            //////////////
            // Check if the current dof is on a face:
            // Loop over its sparsity pattern and find DOFs at the same location.
            vector<int> DOFs_at_ith_location;
            for (int j = I[ldofs[i]]; j < I[ldofs[i] + 1]; j++)
            {
               dist = Distance(ldofs[i], J[j]);
               if (ldofs[i] == DOF_ID)
               {
                  cout << "checking " << J[j] << " " << dist << endl;
               }
               if (dist <= tol && ldofs[i] != J[j]) // dont include the ith DOF
               {
                  DOFs_at_ith_location.push_back(J[j]);

                  // Now look over the sparcity pattern of J[j] to find more
                  // dofs at the same location
                  // (adds diagonal neighbors, if they are on the same mpi task).
                  const int d = J[j];
                  bool is_new = true;
                  for (int jj = I[d]; jj < I[d+1]; jj++)
                  {
                     if (J[jj] == ldofs[i]) { continue; }
                     for (int dd = 0; dd < (int)DOFs_at_ith_location.size(); dd++)
                     {
                        if (J[jj] == DOFs_at_ith_location[dd])
                        { is_new = false; break; }
                     }
                     if (is_new && Distance(d, J[jj]) < tol)
                     { DOFs_at_ith_location.push_back(J[jj]); }
                  }
               }
            }
            if (ldofs[i] == DOF_ID)
            {
               cout << "same location " << DOFs_at_ith_location.size() << endl;
            }
            // Loop over the dofs at i-th location; for each, loop over DOFs
            // local on its cell to find those within dist(1).
            // Note that distance has to be measured on the reference element.
            for (int it = 0; it < (int) DOFs_at_ith_location.size(); it++)
            {
               int dof = DOFs_at_ith_location[it];
               if (dof < NDOFs)
               {
                  const int cell_id = dof / n_dofs;
                  fes->GetElementDofs(cell_id, ldofs_external);
                  fe_external = fes->GetFE(cell_id);
               }
               /* else
               {
                  const int cell_id = dof / n_dofs - num_cells;
                  fes->GetFaceNbrElementVDofs(cell_id, ldofs_external);
                  fe_external = fes->GetFaceNbrFE(cell_id);

                  for (int j = 0; j < ldofs.Size(); j++)
                  {
                     ldofs_external[j] += NDOFs;
                  }
               }*/ // for parallel

               int n_dofs_external = fe_external->GetDof();
               const IntegrationRule &ir_ext = fe_external->GetNodes();
               for (int j = 0; j < n_dofs_external; j++) // here j is local
               {
                  bool is_new = true;
                  for (int dd = 0; dd < (int)map_for_bounds[ldofs[i]].size(); dd++)
                  {
                     if (ldofs_external[j] == map_for_bounds[ldofs[i]][dd])
                     { is_new = false; break; }
                  }

                  int loc_index = dof % n_dofs;
                  if (is_new &&
                      distance_(ir_ext.IntPoint(loc_index),
                                ir_ext.IntPoint(j)) <= dist_level)
                  {
                     map_for_bounds[ldofs[i]].push_back(ldofs_external[j]);
                  }
               }
            }
            if (ldofs[i] == DOF_ID)
            {
               cout << " --- " << endl;
               for (int j = 0; j < (int)map_for_bounds[DOF_ID].size(); j++)
               {
                  cout << map_for_bounds[DOF_ID][j] << endl;
               }
            }
         }
      }
   }
};

class FluxCorrectedTransport
{
private:
   FiniteElementSpace* fes;

public:
   // Constructor builds structures required for low order scheme
   FluxCorrectedTransport(const MONOTYPE _monoType, bool &_isSubCell,
                          FiniteElementSpace* _fes,
                          const SparseMatrix &K, VectorFunctionCoefficient &coef, SolutionBounds &_bnds) :
      fes(_fes), monoType(_monoType), bnds(_bnds) // TODO initialize KpD überlegen
   {
      if (_monoType == None)
      {
         return;
      }
      
      // Compute the lumped mass matrix algebraicly
      BilinearForm m(fes);
      m.AddDomainIntegrator(new LumpedIntegrator(new MassIntegrator));
      m.Assemble();
      m.Finalize();
      m.SpMat().GetDiag(lumpedM);
      
      if ((_monoType == DiscUpw) || (_monoType == DiscUpw_FS))
      {
         Mesh *mesh = fes->GetMesh();
         const FiniteElement &dummy = *fes->GetFE(0);
         int dim = mesh->Dimension(), ne = mesh->GetNE(), nd = dummy.GetDof();
         
         // fill the dofs array to access the correct dofs for boundaries later
         dummy.ExtractBdrDofs(dofs);
         int numBdrs = dofs.Width();
         int numDofs = dofs.Height();
         
         bdrIntLumped.SetSize(ne*nd, numBdrs); bdrIntLumped = 0.;
         bdrInt.SetSize(ne*nd, nd*numBdrs); bdrInt = 0.;
         bdrIntNeighbor.SetSize(ne*nd, nd*numBdrs); bdrIntNeighbor = 0.;
         neighborDof.SetSize(ne*numDofs, numBdrs);
         
         ////////////////////////////
         // Boundary contributions //
         ////////////////////////////
         FaceElementTransformations *Trans;
         // use the first mesh boundary with a neighbor as indicator
         for (int i = 0; i < mesh->GetNumFaces(); i++)
         {
            Trans = mesh->GetFaceElementTransformations(i);
            if (Trans->Elem2No >= 0)
               break;
            // NOTE: The case that the simulation is performed on a single element 
            //       and all boundaries are non-periodic is not covered
         }
         // qOrdF is chosen such that L2-norm of basis functions is computed accurately. TODO this is repeating!
         int qOrdF = std::max(Trans->Elem1->OrderW(), Trans->Elem2->OrderW()) + 2* dummy.GetOrder();
         const IntegrationRule *irF1 = &IntRules.Get(Trans->FaceGeom, qOrdF);
         
         for (int k = 0; k < ne; k++)
            preprocessFluxLumping(fes, coef, k, irF1);
         
         if (_isSubCell) // TODO rename
         {
            BilinearForm prec(fes);
            prec.AddDomainIntegrator(new PrecondConvectionIntegrator(coef, -1.0));
            prec.Assemble(0);
            prec.Finalize(0);

            SparseMatrix kPrec(prec.SpMat());
            
            if (dim==1)
            {
               BilinearForm bdrTerms(fes);
               bdrTerms.AddInteriorFaceIntegrator(
                  new TransposeIntegrator(new DGTraceIntegrator(coef, 1.0, -0.5)));
               bdrTerms.AddBdrFaceIntegrator(
                  new TransposeIntegrator(new DGTraceIntegrator(coef, 1.0, -0.5)));
               bdrTerms.Assemble(0);
               bdrTerms.Finalize(0);
            
               KpD = bdrTerms.SpMat();
            }
            else
               KpD = kPrec;
            
            ComputeDiscreteUpwindingMatrix(prec.SpMat(), kPrec);
            
            if (dim==1)
               KpD += prec.SpMat();

            KpD += kPrec;
         }
         else
         {
            BilinearForm Rho(fes);
            Rho.AddDomainIntegrator(new ConvectionIntegrator(coef, -1.0));
            Rho.Assemble(0); // TODO find a way to avoid if (dim==1)
            Rho.Finalize(0); // TODO repeating
            KpD = Rho.SpMat();
            ComputeDiscreteUpwindingMatrix(Rho.SpMat(), KpD);
            KpD += Rho.SpMat();
         }
      }
      else if ((_monoType == Rusanov) || (_monoType == Rusanov_FS))
      {
         ComputeDiffusionCoefficient(fes, coef);
      }
      else if ( (_monoType == ResDist) || (_monoType == ResDist_FS) 
               || (_monoType == ResDist_Lim) || (_monoType == ResDist_LimMass) )
      {
         ComputeResidualWeights(fes, coef, _isSubCell);
         isSubCell = _isSubCell; // TODO begruenden
      }
   }

   // Utility function to build a map to the offset of the symmetric entry in a sparse matrix
   Array<int> SparseMatrix_Build_smap(const SparseMatrix &A)
   {
      // assuming that A is finalized
      const int *I = A.GetI(), *J = A.GetJ(), n = A.Size();
      Array<int> smap;
      smap.SetSize(I[n]);

      for (int row = 0, j = 0; row < n; row++)
      {
         for (int end = I[row+1]; j < end; j++)
         {
            int col = J[j];
            // find the offset, _j, of the (col,row) entry and store it in smap[j]:
            for (int _j = I[col], _end = I[col+1]; true; _j++)
            {
               if (_j == _end)
               {
                  mfem_error("SparseMatrix_Build_smap");
               }

               if (J[_j] == row)
               {
                  smap[j] = _j;
                  break;
               }
            }
         }
      }
      return smap;
   }

   void ComputeDiscreteUpwindingMatrix(const SparseMatrix& K, SparseMatrix& D)
   {
      const int s1 = K.Size();
      int* Ip = K.GetI();
      int* Jp = K.GetJ();
      double* Kp = K.GetData();
      Array<int> smap = SparseMatrix_Build_smap(K); // symmetry map

      double* Dp = D.GetData();

      for (int i = 0, k = 0; i < s1; i++)
      {
         double rowsum = 0.;
         for (int end = Ip[i+1]; k < end; k++)
         {
            int j = Jp[k];
            double kij = Kp[k];
            double kji = Kp[smap[k]];
            double dij = fmax(fmax(0.0,-kij),-kji);
            Dp[k] = dij;
            Dp[smap[k]] = dij;
            if (i != j) { rowsum += Dp[k]; }
         }
         D(i,i) = -rowsum;
      }
   }

   void ComputeDiffusionCoefficient(FiniteElementSpace* fes,
                                    VectorFunctionCoefficient &coef)
   {
      enum ESTIMATE { Schwarz, Hoelder1Inf, Hoelder1Inf_Exact, HoelderInf1, HoelderInf1_Exact };
      ESTIMATE est = Schwarz;

      Mesh *mesh = fes->GetMesh();
      int i, j, k, p, qOrdE, qOrdF, nd, numBdrs, numDofs, dim = mesh->Dimension(),
                                                          ne = mesh->GetNE();
      double vn;
      Array< int > bdrs, orientation;

      // use the first mesh element as indicator for the following bunch
      const FiniteElement &dummy = *fes->GetFE(0);
      nd = dummy.GetDof();
      // fill the dofs array to access the correct dofs for boundaries
      dummy.ExtractBdrDofs(dofs);
      numBdrs = dofs.Width();
      numDofs = dofs.Height();

      Vector vval, nor(dim), vec1(dim), vec2(nd), shape(nd), alpha(nd), beta(nd),
             shapeBdr(numDofs);
      DenseMatrix velEval, adjJ(dim,dim), dshape(nd,dim);

      elDiff.SetSize(ne); elDiff = 0.;
      bdrDiff.SetSize(ne, numBdrs); bdrDiff = 0.;

      // use the first mesh element as indicator
      ElementTransformation *tr = mesh->GetElementTransformation(0);
      // Assuming order(u)==order(mesh)
      // Depending on ESTIMATE, beta may be impossible to integrate exactly due to transformation dependent denominator
      // use tr->OrderW() + 2*dummy.GetOrder() + 2*dummy.max(tr->OrderGrad(&dummy), 0) instead
      // appropriate qOrdE for alpha is tr->OrderW() + 2*dummy.GetOrder(), choose max
      qOrdE = tr->OrderW() + 2*dummy.GetOrder() + 2*max(tr->OrderGrad(&dummy), 0);
      const IntegrationRule *ir = &IntRules.Get(dummy.GetGeomType(), qOrdE);

      // use the first mesh boundary as indicator
      FaceElementTransformations *Trans = mesh -> GetFaceElementTransformations(0);
      // qOrdF is chosen such that L2-norm of basis functions is computed accurately.
      // Normal velocity term relies on L^Inf-norm which is approximated
      // by its maximum value in the quadrature points of the same rule.
      if (Trans->Elem1No != 0)
      {
         if (Trans->Elem2No != 0)
         {
            mfem_error("Boundary edge does not belong to this element.");
         }
         else
         {
            qOrdF = Trans->Elem2->OrderW() + 2*dummy.GetOrder();
         }
      }
      else
      {
         qOrdF = Trans->Elem1->OrderW() + 2*dummy.GetOrder();
      }
      const IntegrationRule *irF1 = &IntRules.Get(Trans->FaceGeom, qOrdF);

      for (k = 0; k < ne; k++)
      {
         ///////////////////////////
         // Element contributions //
         ///////////////////////////
         const FiniteElement &el = *fes->GetFE(k);
         tr = mesh->GetElementTransformation(k);

         alpha = 0.; beta = 0.;
         coef.Eval(velEval, *tr, *ir);

         for (p = 0; p < ir->GetNPoints(); p++)
         {
            const IntegrationPoint &ip = ir->IntPoint(p);
            tr->SetIntPoint(&ip);

            el.CalcDShape(ip, dshape);
            CalcAdjugate(tr->Jacobian(), adjJ);
            el.CalcShape(ip, shape);

            velEval.GetColumnReference(p, vval);
            adjJ.Mult(vval, vec1);
            dshape.Mult(vec1, vec2);
            for (j = 0; j < nd; j++)
            {
               switch (est)
               {
                  case Schwarz:
                     // divide due to square in L2-norm
                     beta(j) += ip.weight / tr->Weight() * pow(vec2(j), 2.);
                     alpha(j) += ip.weight * tr->Weight() * pow(shape(j), 2.);
                     break;
                  case Hoelder1Inf:
                     // divide because J^-1 = 1 / |J| adj(J)
                     beta(j) = std::max(beta(j), - vec2(j) / tr->Weight());;
                     alpha(j) += ip.weight * tr->Weight() * shape(j);
                     break;
                  case Hoelder1Inf_Exact:
                     beta(j) = std::max(beta(j), - vec2(j));;
                     alpha(j) += ip.weight * shape(j);
                     break;
                  case HoelderInf1:
                     // divide because J^-1 = 1 / |J| adj(J)
                     beta(j) += ip.weight * std::max(0., -vec2(j) / tr->Weight());
                     alpha(j) = std::max(alpha(j), tr->Weight() * shape(j));
                     break;
                  case HoelderInf1_Exact:
                     beta(j) += ip.weight * std::max(0., -vec2(j));
                     alpha(j) = std::max(alpha(j), shape(j));
                     break;
                  default:
                     mfem_error("Unsupported estimate option.");
               }
            }
         }
         elDiff(k) = std::sqrt(alpha.Max() * beta.Max());

         ////////////////////////////
         // Boundary contributions //
         ////////////////////////////
         if (dim==1)
         {
            mesh->GetElementVertices(k, bdrs);
         }
         else if (dim==2)
         {
            mesh->GetElementEdges(k, bdrs, orientation);
         }
         else if (dim==3)
         {
            mesh->GetElementFaces(k, bdrs, orientation);
         }

         for (i = 0; i < numBdrs; i++)
         {
            Trans = mesh->GetFaceElementTransformations(bdrs[i]);
            vn = 0.; shapeBdr = 0.;

            for (p = 0; p < irF1->GetNPoints(); p++)
            {
               const IntegrationPoint &ip = irF1->IntPoint(p);
               IntegrationPoint eip1;
               Trans->Face->SetIntPoint(&ip);

               if (dim == 1)
               {
                  nor(0) = 2.*eip1.x - 1.0;
               }
               else
               {
                  CalcOrtho(Trans->Face->Jacobian(), nor);
               }

               if (Trans->Elem1No != k)
               {
                  Trans->Loc2.Transform(ip, eip1);
                  el.CalcShape(eip1, shape);
                  Trans->Elem2->SetIntPoint(&eip1);
                  coef.Eval(vval, *Trans->Elem2, eip1);
                  nor *= -1.;
               }
               else
               {
                  Trans->Loc1.Transform(ip, eip1);
                  el.CalcShape(eip1, shape);
                  Trans->Elem1->SetIntPoint(&eip1);
                  coef.Eval(vval, *Trans->Elem1, eip1);
               }

               nor /= nor.Norml2();

               vn = std::max(vn, vval * nor);
               for (j = 0; j < numDofs; j++)
               {
                  shapeBdr(j) += ip.weight * Trans->Face->Weight() * pow(shape(dofs(j,i)), 2.);
               }
            }
            bdrDiff(k,i) = vn * shapeBdr.Max();
         }
      }
   }

   void ComputeResidualWeights(FiniteElementSpace* fes,
                               VectorFunctionCoefficient &coef, bool &isSubCell)
   {
      Mesh *mesh = fes->GetMesh();
      int i, j, k, m, p, nd, dofInd, qOrdF, numBdrs, numDofs,
          numSubcells, numDofsSubcell, dim = mesh->Dimension(), ne = mesh->GetNE();
      DenseMatrix elmat;
      ElementTransformation *tr;
      FaceElementTransformations *Trans;

      // use the first mesh element as indicator for the following bunch
      const FiniteElement &dummy = *fes->GetFE(0);
      nd = dummy.GetDof();
      p = dummy.GetOrder();

      if ((p==1) && isSubCell)
      {
         mfem_warning("Subcell option does not make sense for order 1. Using cell-based scheme."); // TODO also for Rusanov
         isSubCell = false;
      }

      if (dim==1)
      {
         numSubcells = p;
         numDofsSubcell = 2;
      }
      else if (dim==2)
      {
         numSubcells = p*p;
         numDofsSubcell = 4;
      }
      else if (dim==3)
      {
         numSubcells = p*p*p;
         numDofsSubcell = 8;
      }

      // fill the dofs array to access the correct dofs for boundaries later; dofs is not needed here
      dummy.ExtractBdrDofs(dofs);
      numBdrs = dofs.Width();
      numDofs = dofs.Height();
      
      // use the first mesh boundary with a neighbor as indicator
      for (i = 0; i < mesh->GetNumFaces(); i++)
      {
         Trans = mesh->GetFaceElementTransformations(i);
         if (Trans->Elem2No >= 0)
         {
            break;
         }
         // NOTE: The case that the simulation is performed on a single element
         //       and all boundaries are non-periodic is not covered
      }
      // qOrdF is chosen such that L2-norm of basis functions is computed accurately. TODO
      qOrdF = std::max(Trans->Elem1->OrderW(),
                       Trans->Elem2->OrderW()) + 2*dummy.GetOrder();
      const IntegrationRule *irF1 = &IntRules.Get(Trans->FaceGeom, qOrdF);

      BilinearFormIntegrator *fluct;
      fluct = new MixedConvectionIntegrator(coef, -1.0);

      BilinearForm Rho(fes);
      Rho.AddDomainIntegrator(new ConvectionIntegrator(coef, -1.0));
      Rho.Assemble();
      Rho.Finalize();
      fluctMatrix = Rho.SpMat();

      int basis_lor = BasisType::ClosedUniform; // to have a uniformly refined mesh
      Mesh *ref_mesh;
      if (p==1)
      {
         ref_mesh = mesh;
      }
      else if (dim > 1)
      {
         ref_mesh = new Mesh(mesh, p, basis_lor); // TODO delete all news
         ref_mesh->SetCurvature(1);
      }
      else
      {
         ref_mesh = new Mesh(ne*p,
                             1.); // TODO generalize to segments with different length than 1
         ref_mesh->SetCurvature(1);
      }

      const int btype = BasisType::Positive;
      DG_FECollection fec0(0, dim, btype);
      DG_FECollection fec1(1, dim,
                           btype);

      FiniteElementSpace SubFes0(ref_mesh, &fec0);
      FiniteElementSpace SubFes1(ref_mesh, &fec1);

      FillSubcell2CellDof(p, dim);

      fluctSub.SetSize(ne*numSubcells, numDofsSubcell);
      bdrIntLumped.SetSize(ne*nd, numBdrs); bdrIntLumped = 0.;
      bdrInt.SetSize(ne*nd, nd*numBdrs); bdrInt = 0.;
      bdrIntNeighbor.SetSize(ne*nd, nd*numBdrs); bdrIntNeighbor = 0.;
      neighborDof.SetSize(ne*numDofs, numBdrs);

      double dist = 1. / double(p);
      Vector velEval; // TODO rm

      for (k = 0; k < ne; k++)
      {
         ////////////////////////////
         // Boundary contributions //
         ////////////////////////////
         preprocessFluxLumping(fes, coef, k, irF1);

         ///////////////////////////
         // Element contributions //
         ///////////////////////////
         const FiniteElement &el = *fes->GetFE(k);
         const IntegrationRule ir = el.GetNodes();
         for (m = 0; m < numSubcells; m++)
         {
            dofInd = numSubcells*k+m;
            const FiniteElement *el0 = SubFes0.GetFE(dofInd);
            const FiniteElement *el1 = SubFes1.GetFE(dofInd);
            tr = ref_mesh->GetElementTransformation(dofInd);
            fluct->AssembleElementMatrix2(*el1, *el0, *tr, elmat);

            for (j = 0; j < numDofsSubcell; j++)
               fluctSub(dofInd, j) = elmat(0,j);
         }

         tr = mesh->GetElementTransformation(k);
         

      }
      if (p!=1)
      {
         delete ref_mesh;
      }
      delete fluct;
   }
   
   void preprocessFluxLumping(FiniteElementSpace* fes, VectorFunctionCoefficient &coef, const int k, 
                              const IntegrationRule *irF1)
   {
      const FiniteElement &el = *fes->GetFE(k);
      Mesh *mesh = fes->GetMesh();
      
      int i, j, l, m, idx, neighborElem, numBdrs = dofs.Width(), numDofs = dofs.Height(), 
            nd = el.GetDof(), p = el.GetOrder(), dim = mesh->Dimension();
      double vn;
      Array <int> bdrs, orientation;
      FaceElementTransformations *Trans;
      
      Vector vval, nor(dim), shape(nd), shapeNeighbor(nd);
      
      if (dim==1)
         numBdrs = 0; // Nothing needs to be done for 1D boundaries
      else if (dim==2)
         mesh->GetElementEdges(k, bdrs, orientation);
      else if (dim==3)
         mesh->GetElementFaces(k, bdrs, orientation);
      
      FillNeighborDofs(mesh, numDofs, k, nd, p, dim, bdrs);
      
      for (i = 0; i < numBdrs; i++)
      {
         Trans = mesh->GetFaceElementTransformations(bdrs[i]);
         vn = 0.;
         
         for (l = 0; l < irF1->GetNPoints(); l++)
         {
            const IntegrationPoint &ip = irF1->IntPoint(l);
            IntegrationPoint eip1;
            Trans->Face->SetIntPoint(&ip);
            
            if (dim == 1)
               nor(0) = 2.*eip1.x - 1.0;
            else
               CalcOrtho(Trans->Face->Jacobian(), nor);
            
            if (Trans->Elem1No != k)
            {
               Trans->Loc2.Transform(ip, eip1);
               el.CalcShape(eip1, shape);
               Trans->Elem2->SetIntPoint(&eip1);
               coef.Eval(vval, *Trans->Elem2, eip1);
               nor *= -1.;
               Trans->Loc1.Transform(ip, eip1);
               el.CalcShape(eip1, shapeNeighbor);
               
               neighborElem = Trans->Elem1No;
            }
            else
            {
               Trans->Loc1.Transform(ip, eip1);
               el.CalcShape(eip1, shape);
               Trans->Elem1->SetIntPoint(&eip1);
               coef.Eval(vval, *Trans->Elem1, eip1);
               Trans->Loc2.Transform(ip, eip1);
               el.CalcShape(eip1, shapeNeighbor);
               
               neighborElem = Trans->Elem2No;
            }
            
            nor /= nor.Norml2();
            vn = min(0., vval * nor);
            
            for(j = 0; j < numDofs; j++)
            {
               bdrIntLumped(k*nd+dofs(j,i),i) -= ip.weight * 
               Trans->Face->Weight() * shape(dofs(j,i)) * vn;
               
               for (m = 0; m < numDofs; m++)
               {
                  bdrInt(k*nd+dofs(j,i),i*nd+dofs(m,i)) += ip.weight * 
                  Trans->Face->Weight() * shape(dofs(j,i)) * shape(dofs(m,i)) * vn;
                  
                  if (neighborElem != 0)
                     idx = ((int)(neighborDof(k*numDofs+m,i))) % (neighborElem*nd);
                  else
                     idx = neighborDof(k*numDofs+m,i);
                  
                  bdrIntNeighbor(k*nd+dofs(j,i),i*nd+dofs(m,i)) += ip.weight * 
                  Trans->Face->Weight() * shape(dofs(j,i)) * shapeNeighbor(idx) * vn;
               }
            }
         }
      }
   }

   // Computes the element-global indices from the indices of the subcell and the indices
   // of dofs on the subcell. No support for triangles and tetrahedrons.
   void FillSubcell2CellDof(int p,int dim)
   {
      int m, j, numSubcells, numDofsSubcell;
      if (dim==1)
      {
         numSubcells = p;
         numDofsSubcell = 2;
      }
      else if (dim==2)
      {
         numSubcells = p*p;
         numDofsSubcell = 4;
      }
      else if (dim==3)
      {
         numSubcells = p*p*p;
         numDofsSubcell = 8;
      }

      subcell2CellDof.SetSize(numSubcells, numDofsSubcell);
      for (m = 0; m < numSubcells; m++)
      {
         for (j = 0; j < numDofsSubcell; j++)
         {
            if (dim == 1)
            {
               subcell2CellDof(m,j) = m + j;
            }
            else if (dim == 2)
            {
               switch (j)
               {
                  case 0: subcell2CellDof(m,j) =  m + (m / p); break;
                  case 1: subcell2CellDof(m,j) =  m + (m / p) + 1; break;
                  case 2: subcell2CellDof(m,j) =  m + (m / p) + p + 1; break;
                  case 3: subcell2CellDof(m,j) =  m + (m / p) + p + 2; break;
               }
            }
            else if (dim == 3)
            {
               switch (j)
               {
                  case 0: subcell2CellDof(m,j) =  m + (m / p) + (p+1) * (m / (p*p)); break;
                  case 1: subcell2CellDof(m,j) =  m + (m / p) + (p+1) * (m / (p*p)) + 1; break;
                  case 2: subcell2CellDof(m,j) =  m + (m / p) + (p+1) * (m / (p*p)) + p + 1;
                     break;
                  case 3: subcell2CellDof(m,j) =  m + (m / p) + (p+1) * (m / (p*p)) + p + 2;
                     break;
                  case 4: subcell2CellDof(m,j) =  m + (m / p) + (p+1) * (m / (p*p)) + (p+1)*(p+1);
                     break;
                  case 5: subcell2CellDof(m,
                                             j) =  m + (m / p) + (p+1) * (m / (p*p)) + (p+1)*(p+1) + 1; break;
                  case 6: subcell2CellDof(m,
                                             j) =  m + (m / p) + (p+1) * (m / (p*p)) + (p+1)*(p+1) + p + 1; break;
                  case 7: subcell2CellDof(m,
                                             j) =  m + (m / p) + (p+1) * (m / (p*p)) + (p+1)*(p+1) + p + 2; break;
               }
            }
         }
      }
   }

   void FillNeighborDofs(Mesh *mesh, int numDofs, int k, int nd, int p, int dim,
                         Array <int> bdrs)
   {
      int j, neighborElem;
      FaceElementTransformations *Trans;

      if (dim == 1) { return; } // no need to take care of boundary terms
      else if (dim == 2)
      {
         for (j = 0; j < numDofs; j++)
         {
            Trans = mesh->GetFaceElementTransformations(bdrs[0]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j, 0) = neighborElem*nd + (p+1)*p+j;

            Trans = mesh->GetFaceElementTransformations(bdrs[1]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j, 1) = neighborElem*nd + (p+1)*j;

            Trans = mesh->GetFaceElementTransformations(bdrs[2]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j, 2) = neighborElem*nd + j;

            Trans = mesh->GetFaceElementTransformations(bdrs[3]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j, 3) = neighborElem*nd + (p+1)*j+p;
         }
      }
      else // dim == 3
      {
         for (j = 0; j < numDofs; j++)
         {
            Trans = mesh->GetFaceElementTransformations(bdrs[0]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j, 0) = neighborElem*nd + (p+1)*(p+1)*p+j;

            Trans = mesh->GetFaceElementTransformations(bdrs[1]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j,
                        1) = neighborElem*nd + (j/(p+1))*(p+1)*(p+1) + (p+1)*p+(j%(p+1));

            Trans = mesh->GetFaceElementTransformations(bdrs[2]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j, 2) = neighborElem*nd + j*(p+1);

            Trans = mesh->GetFaceElementTransformations(bdrs[3]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j,
                        3) = neighborElem*nd + (j/(p+1))*(p+1)*(p+1) + (j%(p+1));

            Trans = mesh->GetFaceElementTransformations(bdrs[4]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j, 4) = neighborElem*nd + (j+1)*(p+1)-1;

            Trans = mesh->GetFaceElementTransformations(bdrs[5]);
            if (Trans->Elem1No == k)
            {
               neighborElem = Trans->Elem2No;
            }
            else
            {
               neighborElem = Trans->Elem1No;
            }

            neighborDof(k*numDofs+j, 5) = neighborElem*nd + j;
         }
      }
   }

   // Destructor
   ~FluxCorrectedTransport() { }

   // member variables that need to be accessed during time-stepping
   const MONOTYPE monoType;

   Vector lumpedM, elDiff;
   bool isSubCell;
   SparseMatrix KpD, fluctMatrix;
   Array<int> numSubCellsForNode;
   DenseMatrix bdrDiff, dofs, neighborDof, subcell2CellDof, bdrIntNeighbor, bdrInt,
               bdrIntLumped, fluctSub;
   SolutionBounds &bnds;
};


/** A time-dependent operator for the right-hand side of the ODE. The DG weak
    form of du/dt = -v.grad(u) is M du/dt = K u + b, where M and K are the mass
    and advection matrices, and b describes the flow on the boundary. This can
    be written as a general ODE, du/dt = M^{-1} (K u + b), and this class is
    used to evaluate the right-hand side. */
class FE_Evolution : public TimeDependentOperator
{
private:
   FiniteElementSpace* fes;
   SparseMatrix &M, &K;
   const Vector &b;
   DSmoother M_prec;
   CGSolver M_solver;

   mutable Vector z;

   double dt;
   const FluxCorrectedTransport &fct;

public:
   FE_Evolution(FiniteElementSpace* fes, SparseMatrix &_M, SparseMatrix &_K,
                const Vector &_b, FluxCorrectedTransport &_fct);

   virtual void Mult(const Vector &x, Vector &y) const;

   virtual void SetDt(double _dt) { dt = _dt; }

   virtual void LumpFluxTerms(int k, int nd, const Vector &x, Vector &y, const Vector alpha) const;

   virtual void ComputeHighOrderSolution(const Vector &x, Vector &y) const;
   virtual void ComputeLowOrderSolution(const Vector &x, Vector &y) const;
   virtual void ComputeFCTSolution(const Vector &x, const Vector &yH,
                                   const Vector &yL, Vector &y) const;
   virtual void ComputeLimitedSolution(const Vector &x, Vector &y) const;

   virtual ~FE_Evolution() { }
};


int main(int argc, char *argv[])
{
   // 1. Parse command-line options.
   problem = 4;
   const char *mesh_file = "../data/periodic-square.mesh";
   int ref_levels = 2;
   int order = 3;
   int ode_solver_type = 3;
   MONOTYPE monoType = ResDist_Lim;
   bool isSubCell = true; // TODO maybe implement subcell==false by setting gamma to zero?
   STENCIL stencil = Full; // must be Full for ResDist_Lim
   double t_final = 4.0;
   double dt = 0.005;
   bool visualization = true;
   bool visit = false;
   bool binary = false;
   int vis_steps = 100;

   int precision = 8;
   cout.precision(precision);

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&problem, "-p", "--problem",
                  "Problem setup to use. See options in velocity_function().");
   args.AddOption(&ref_levels, "-r", "--refine",
                  "Number of times to refine the mesh uniformly.");
   args.AddOption(&order, "-o", "--order",
                  "Order (degree) of the finite elements.");
   args.AddOption(&ode_solver_type, "-s", "--ode-solver",
                  "ODE solver: 1 - Forward Euler,\n\t"
                  "            2 - RK2 SSP, 3 - RK3 SSP, 4 - RK4, 6 - RK6.");
   args.AddOption((int*)(&monoType), "-mt", "--monoType",
                  "Type of monotonicity treatment: 0 - no monotonicity treatment,\n\t"
                  "                                1 - discrete upwinding - low order,\n\t"
                  "                                2 - discrete upwinding - FCT,\n\t"
                  "                                3 - Rusanov - low order,\n\t"
                  "                                4 - Rusanov - FCT,\n\t"
                  "                                5 - residual distribution scheme (matrix-free) - low order,\n\t"
                  "                                6 - residual distribution scheme (matrix-free) - FCT."); // TODO
   args.AddOption((int*)(&stencil), "-st", "--stencil",
                  "Type of stencil for high order scheme: 0 - all neighbors,\n\t"
                  "                                       1 - closest neighbors,\n\t"
                  "                                       2 - closest plus diagonal neighbors.");
   args.AddOption(&t_final, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&dt, "-dt", "--time-step",
                  "Time step.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&visit, "-visit", "--visit-datafiles", "-no-visit",
                  "--no-visit-datafiles",
                  "Save data files for VisIt (visit.llnl.gov) visualization.");
   args.AddOption(&binary, "-binary", "--binary-datafiles", "-ascii",
                  "--ascii-datafiles",
                  "Use binary (Sidre) or ascii format for VisIt data files.");
   args.AddOption(&vis_steps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.Parse();
   if (!args.Good())
   {
      args.PrintUsage(cout);
      return 1;
   }
   args.PrintOptions(cout);

   // 2. Read the mesh from the given mesh file. We can handle geometrically
   //    periodic meshes in this code.
   Mesh *mesh = new Mesh(mesh_file, 1, 1);
   int dim = mesh->Dimension();

   // 3. Define the ODE solver used for time integration. Several explicit
   //    Runge-Kutta methods are available.
   ODESolver *ode_solver = NULL;
   switch (ode_solver_type)
   {
      case 1: ode_solver = new ForwardEulerSolver; break;
      case 2: ode_solver = new RK2Solver(1.0); break;
      case 3: ode_solver = new RK3SSPSolver; break;
      case 4: ode_solver = new RK4Solver; break;
      case 6: ode_solver = new RK6Solver; break;
      default:
         cout << "Unknown ODE solver type: " << ode_solver_type << '\n';
         delete mesh;
         return 3;
   }

   // 4. Refine the mesh to increase the resolution. In this example we do
   //    'ref_levels' of uniform refinement, where 'ref_levels' is a
   //    command-line parameter. If the mesh is of NURBS type, we convert it to
   //    a (piecewise-polynomial) high-order mesh.
   for (int lev = 0; lev < ref_levels; lev++)
   {
      mesh->UniformRefinement();
   }
   if (mesh->NURBSext)
   {
      mesh->SetCurvature(max(order, 1));
   }
   mesh->GetBoundingBox(bb_min, bb_max, max(order, 1));

   // 5. Define the discontinuous DG finite element space of the given
   //    polynomial order on the refined mesh.
   const int btype = BasisType::Positive;
   DG_FECollection fec(order, dim, btype);
   FiniteElementSpace fes(mesh, &fec);

   if (monoType != None)
   {
      if (((int)monoType != monoType) || (monoType < 0) || (monoType > 8))
      {
         cout << "Unsupported option for monotonicity treatment." << endl;
         delete mesh;
         delete ode_solver;
         return 5;
      }
      if ((btype != 2) && (monoType > 2))
      {
         cout << "Matrix-free monotonicity treatment requires use of Bernstein basis." <<
              endl;
         delete mesh;
         delete ode_solver;
         return 5;
      }
      if (order == 0)
      {
         mfem_error("No need to use monotonicity treatment for polynomial order 0.");
      }
   }

   cout << "Number of unknowns: " << fes.GetVSize() << endl;

   // 6. Set up and assemble the bilinear and linear forms corresponding to the
   //    DG discretization. The DGTraceIntegrator involves integrals over mesh
   //    interior faces.
   //    Also prepare for the use of low and high order schemes.
   VectorFunctionCoefficient velocity(dim, velocity_function);
   FunctionCoefficient inflow(inflow_function);
   FunctionCoefficient u0(u0_function);

   BilinearForm m(&fes);
   m.AddDomainIntegrator(new MassIntegrator);
   BilinearForm k(&fes);
   k.AddDomainIntegrator(new ConvectionIntegrator(velocity, -1.0));
   k.AddInteriorFaceIntegrator(
      new TransposeIntegrator(new DGTraceIntegrator(velocity, 1.0, -0.5)));
   k.AddBdrFaceIntegrator(
      new TransposeIntegrator(new DGTraceIntegrator(velocity, 1.0, -0.5)));

   LinearForm b(&fes);
   b.AddBdrFaceIntegrator(
      new BoundaryFlowIntegrator(inflow, velocity, -1.0, -0.5));

   m.Assemble();
   m.Finalize();
   int skip_zeros = 0;
   k.Assemble(skip_zeros);
   k.Finalize(skip_zeros);
   b.Assemble();

   // Compute data required to easily find the min-/max-values for the high order scheme
   SolutionBounds bnds(&fes, k, stencil);
   // Precompute data required for high and low order schemes
   FluxCorrectedTransport fct(monoType, isSubCell, &fes, k.SpMat(), velocity,
                              bnds);

   // 7. Define the initial conditions, save the corresponding grid function to
   //    a file and (optionally) save data in the VisIt format and initialize
   //    GLVis visualization.
   GridFunction u(&fes);
   u.ProjectCoefficient(u0);

   {
      ofstream omesh("ex9.mesh");
      omesh.precision(precision);
      mesh->Print(omesh);
      ofstream osol("ex9-init.gf");
      osol.precision(precision);
      u.Save(osol);
   }

   // Create data collection for solution output: either VisItDataCollection for
   // ascii data files, or SidreDataCollection for binary data files.
   DataCollection *dc = NULL;
   if (visit)
   {
      if (binary)
      {
#ifdef MFEM_USE_SIDRE
         dc = new SidreDataCollection("Example9", mesh);
#else
         MFEM_ABORT("Must build with MFEM_USE_SIDRE=YES for binary output.");
#endif
      }
      else
      {
         dc = new VisItDataCollection("Example9", mesh);
         dc->SetPrecision(precision);
      }
      dc->RegisterField("solution", &u);
      dc->SetCycle(0);
      dc->SetTime(0.0);
      dc->Save();
   }

   socketstream sout;
   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      sout.open(vishost, visport);
      if (!sout)
      {
         cout << "Unable to connect to GLVis server at "
              << vishost << ':' << visport << endl;
         visualization = false;
         cout << "GLVis visualization disabled.\n";
      }
      else
      {
         sout.precision(precision);
         sout << "solution\n" << *mesh << u;
         sout << "pause\n";
         sout << flush;
         cout << "GLVis visualization paused."
              << " Press space (in the GLVis window) to resume it.\n";
      }
   }

   // check for conservation TODO
   Vector tmp;
   tmp.SetSize(u.Size());
   m.SpMat().Mult(u, tmp);
   double initialMass = tmp.Sum();

   // 8. Define the time-dependent evolution operator describing the ODE
   //    right-hand side, and perform time-integration (looping over the time
   //    iterations, ti, with a time-step dt).
   FE_Evolution adv(&fes, m.SpMat(), k.SpMat(), b, fct);

   double t = 0.0;
   adv.SetTime(t);
   ode_solver->Init(adv);

   bool done = false;
   for (int ti = 0; !done; )
   {
      // compute solution bounds
      if ((monoType > 0) && (monoType < 7))
      {
         fct.bnds.Compute(k.SpMat(), u);
      }
      adv.SetDt(dt);

      double dt_real = min(dt, t_final - t);
      ode_solver->Step(u, t, dt_real);
      ti++;

      done = (t >= t_final - 1.e-8*dt);

      if (done || ti % vis_steps == 0)
      {
         cout << "time step: " << ti << ", time: " << t << endl;

         if (visualization)
         {
            sout << "solution\n" << *mesh << u << flush;
         }

         if (visit)
         {
            dc->SetCycle(ti);
            dc->SetTime(t);
            dc->Save();
         }
      }
   }

   // 9. Save the final solution. This output can be viewed later using GLVis:
   //    "glvis -m ex9.mesh -g ex9-final.gf".
   {
      ofstream osol("ex9-final.gf");
      osol.precision(precision);
      u.Save(osol);
   }

   // check for conservation TODO
   m.SpMat().Mult(u, tmp);
   double finalMass = tmp.Sum();
   cout << "initial mass: " << initialMass << ", final mass: " << finalMass
         << ", computed with lumped mass matrix: " << fct.lumpedM * u << 
         ", mass loss: " << initialMass - finalMass << endl;

   // 10. Free the used memory.
   delete mesh;
   delete ode_solver;
   delete dc;

   return 0;
}

void FE_Evolution::LumpFluxTerms(int k, int nd, const Vector &x, Vector &y, const Vector alpha) const
{
   int i, j, m, idx, dofInd, numBdrs(fct.dofs.Width()), numDofs(fct.dofs.Height());
   double xNeighbor, sumLumpedFluxP, sumLumpedFluxN, sumFluxP, sumFluxN, weightP, weightN, eps = 1.E-15;
   Vector lumpedFluxP(numDofs), lumpedFluxN(numDofs), totalFlux(numDofs);
   
   for (j = 0; j < numBdrs; j++)
   {
      sumLumpedFluxP = sumLumpedFluxN = 0.;
      for (i = 0; i < numDofs; i++)
      {
         dofInd = k*nd+fct.dofs(i,j);
         idx = fct.neighborDof(k*numDofs+i,j);
         xNeighbor = idx < 0 ? 0. : x(idx);
         lumpedFluxP(i) = max(0., xNeighbor - x(dofInd)) * fct.bdrIntLumped(dofInd, j);
         lumpedFluxN(i) = min(0., xNeighbor - x(dofInd)) * fct.bdrIntLumped(dofInd, j);
         sumLumpedFluxP += lumpedFluxP(i);
         sumLumpedFluxN += lumpedFluxN(i);
         totalFlux(i) = 0.;
         for (m = 0; m < numDofs; m++)
         {
            idx = fct.neighborDof(k*numDofs+m,j);
            xNeighbor = idx < 0 ? 0. : x(idx);
            totalFlux(i) += fct.bdrInt(dofInd, j*nd+fct.dofs(m,j)) * x(k*nd+fct.dofs(m,j))
                          - fct.bdrIntNeighbor(dofInd, j*nd+fct.dofs(m,j)) * xNeighbor;
         }
         y(k*nd+fct.dofs(i,j)) += alpha(fct.dofs(i,j)) * totalFlux(i);
      }
               
      for (i = 0; i < numDofs; i++)
      {
         weightP = lumpedFluxP(i) / (sumLumpedFluxP + eps);
         weightN = lumpedFluxN(i) / (sumLumpedFluxN - eps);
         for (m = 0; m < numDofs; m++)
         {
            if (totalFlux(m) > eps)
            {
               y(k*nd+fct.dofs(i,j)) += (1. - alpha(fct.dofs(m,j))) * weightP * totalFlux(m);
            }
            else if (totalFlux(m) < -eps)
            {
               y(k*nd+fct.dofs(i,j)) += (1. - alpha(fct.dofs(m,j))) * weightN * totalFlux(m);
            }
         }
      }
   }
}

void FE_Evolution::ComputeLowOrderSolution(const Vector &x, Vector &y) const
{
   if ((fct.monoType == DiscUpw) || (fct.monoType == DiscUpw_FS))
   {
      Vector alpha;
      // Discretization AND monotonicity terms
      fct.KpD.Mult(x, y);
      y += b;
      for (int k = 0; k < fes->GetNE(); k++)
      {
         const FiniteElement &el = *fes->GetFE(k);
         int nd = el.GetDof();
         alpha.SetSize(nd); alpha = 0.; // TODO
         
         if (fct.isSubCell)
         {
            LumpFluxTerms(k, nd, x, y, alpha);
         }

         for (int j = 0; j < nd; j++)
         {
            int dofInd = k*nd+j;
            y(dofInd) /= fct.lumpedM(dofInd);
         }
      }
   }
   else if ((fct.monoType == Rusanov) || (fct.monoType == Rusanov_FS))
   {
      Mesh *mesh = fes->GetMesh();
      int i, j, k, nd, dofInd, dim(mesh->Dimension()), numDofs(fct.dofs.Height()),
          numBdrs = fct.dofs.Width();
      Array< int > bdrs, orientation;
      double uSum;

      // Discretization terms
      K.Mult(x, z);
      z += b;

      // Monotonicity terms
      for (k = 0; k < mesh->GetNE(); k++)
      {
         const FiniteElement &el = *fes->GetFE(k);
         nd = el.GetDof();

         ////////////////////////////
         // Boundary contributions //
         ////////////////////////////
         for (i = 0; i < numBdrs; i++)
         {
            uSum = 0.;
            for (j = 0; j < numDofs; j++)
            {
               uSum += x(k*nd+fct.dofs(i,j));
            }

            // boundary update
            for (j = 0; j < numDofs; j++)
            {
               z(k*nd+fct.dofs(i,j)) += fct.bdrDiff(k,i)*(uSum - numDofs*x(k*nd+fct.dofs(i,
                                                                                         j)));
            }
         }
         ///////////////////////////
         // Element contributions //
         ///////////////////////////
         uSum = 0.;
         for (j = 0; j < nd; j++)
         {
            uSum += x(k*nd+j);
         }

         for (j = 0; j < nd; j++)
         {
            // element update and inversion of lumped mass matrix
            dofInd = k*nd+j;
            y(dofInd) = ( z(dofInd) + fct.elDiff(k)*(uSum - nd*x(dofInd)) ) / fct.lumpedM(
                           dofInd);
         }
      }
   }
   else if ( (fct.monoType == ResDist) || (fct.monoType == ResDist_FS) 
            || (fct.monoType == ResDist_Lim) || (fct.monoType == ResDist_LimMass))
   {
      Mesh *mesh = fes->GetMesh();
      int i, j, k, m, p, nd, dofInd, dofInd2, numSubcells, numDofsSubcell,
          ne(fes->GetNE()), dim(mesh->Dimension());
      double xMax, xMin, xSum, xNeighbor, sumRhoSubcellP, sumRhoSubcellN, sumWeightsP,
             sumWeightsN, weightP, weightN, rhoP, rhoN, fluct, fluctSubcellP, fluctSubcellN, gammaP, gammaN,
             minGammaP, minGammaN, gamma = 1.E2, beta = 10., eps = 1.E-15;
      Vector xMaxSubcell, xMinSubcell, sumWeightsSubcellP, sumWeightsSubcellN,
             rhoSubcellP, rhoSubcellN, nodalWeightsP, nodalWeightsN, alpha;

      if (fct.monoType > 6)
      {
         fct.bnds.Compute(K, x);
      }
       
      // Discretization terms
      y = b;
      fct.fluctMatrix.Mult(x, z);
      if (dim==1)
      {
         K.AddMult(x, y);
         y -= z;
      }

      // Monotonicity terms
      for (k = 0; k < ne; k++)
      {
         const FiniteElement &el = *fes->GetFE(k);
         nd = el.GetDof();
         p = el.GetOrder();

         if (dim==1)
         {
            numSubcells = p;
            numDofsSubcell = 2;
         }
         else if (dim==2)
         {
            numSubcells = p*p;
            numDofsSubcell = 4;
         }
         else if (dim==3)
         {
            numSubcells = p*p*p;
            numDofsSubcell = 8;
         }

         ///////////////////////////
         // Element contributions //
         ///////////////////////////
         xMin = numeric_limits<double>::infinity();
         xMax = -xMin;
         rhoP = rhoN = xSum = 0.;
         alpha.SetSize(nd); alpha = 0.;

         for (j = 0; j < nd; j++)
         {
            dofInd = k*nd+j;
            xMax = max(xMax, x(dofInd));
            xMin = min(xMin, x(dofInd));
            xSum += x(dofInd);
            rhoP += max(0., z(dofInd));
            rhoN += min(0., z(dofInd)); // TODO just needed for subcells
         }
         
         if ( (fct.monoType == ResDist_Lim) || (fct.monoType == ResDist_LimMass) )
         {
            for (j = 0; j < nd; j++)
            {
               dofInd = k*nd+j;
               alpha(j) = min( 1., beta * min(fct.bnds.x_max(dofInd) - x(dofInd), x(dofInd) - fct.bnds.x_min(dofInd)) 
                                       / (max(xMax - x(dofInd), x(dofInd) - xMin) + eps) );
            }
         }
         
         ////////////////////////////
         // Boundary contributions //
         ////////////////////////////
         if (dim > 1)// Nothing needs to be done for 1D boundaries (due to Bernstein basis)
            LumpFluxTerms(k, nd, x, y, alpha);
         
         sumWeightsP = nd*xMax - xSum + eps;
         sumWeightsN = nd*xMin - xSum - eps;
         
         if (fct.isSubCell)
         {
            rhoSubcellP.SetSize(numSubcells);
            rhoSubcellN.SetSize(numSubcells);
            xMaxSubcell.SetSize(numSubcells);
            xMinSubcell.SetSize(numSubcells);
            nodalWeightsP.SetSize(nd);
            nodalWeightsN.SetSize(nd);
            sumWeightsSubcellP.SetSize(numSubcells);
            sumWeightsSubcellN.SetSize(numSubcells);
            for (m = 0; m < numSubcells; m++)
            {
               xMinSubcell(m) = numeric_limits<double>::infinity();
               xMaxSubcell(m) = -xMinSubcell(m);
               fluct = xSum = 0.;
               for (i = 0; i < numDofsSubcell;
                    i++) // compute min-/max-values and the fluctuation for subcells
               {
                  dofInd = k*nd + fct.subcell2CellDof(m, i);
                  fluct += fct.fluctSub(k*numSubcells+m,i) * x(dofInd);
                  xMaxSubcell(m) = max(xMaxSubcell(m), x(dofInd));
                  xMinSubcell(m) = min(xMinSubcell(m), x(dofInd));
                  xSum += x(dofInd);
               }
               sumWeightsSubcellP(m) = numDofsSubcell * xMaxSubcell(m) - xSum + eps;
               sumWeightsSubcellN(m) = numDofsSubcell * xMinSubcell(m) - xSum - eps;

               rhoSubcellP(m) = max(0., fluct);
               rhoSubcellN(m) = min(0., fluct);
            }
            sumRhoSubcellP = rhoSubcellP.Sum();
            sumRhoSubcellN = rhoSubcellN.Sum();
            nodalWeightsP = 0.; nodalWeightsN = 0.;
            
            for (m = 0; m < numSubcells; m++)
            {
               for (i = 0; i < numDofsSubcell;
                    i++)
               {
                  int loc = fct.subcell2CellDof(m, i);
                  dofInd = k*nd + loc;
                  nodalWeightsP(loc) += rhoSubcellP(m) * ((xMaxSubcell(m) - x(dofInd)) / sumWeightsSubcellP(m));
                  nodalWeightsN(loc) += rhoSubcellN(m) * ((xMinSubcell(m) - x(dofInd)) / sumWeightsSubcellN(m));
               }
            }
         }

         for (i = 0; i < nd; i++)
         {
            dofInd = k*nd+i;
            weightP = (xMax - x(dofInd)) / sumWeightsP;
            weightN = (xMin - x(dofInd)) / sumWeightsN;
            
            if (fct.isSubCell)
            {
               double auxP = gamma / (rhoP + eps);
               weightP *= 1. - min(auxP * sumRhoSubcellP, 1.);
               weightP += min(auxP, 1. / (sumRhoSubcellP + eps)) * nodalWeightsP(i);
               
               double auxN = gamma / (rhoN - eps);
               weightN *= 1. - min(auxN * sumRhoSubcellN, 1.);
               weightN += max(auxN, 1. / (sumRhoSubcellN - eps)) * nodalWeightsN(i);
            }
            
            for (j = 0; j < nd; j++)
            {
               dofInd2 = k*nd+j;
               if (z(dofInd2) > eps)
               {
                  y(dofInd) += (1. - alpha(j)) * weightP * z(dofInd2);
               }
               else if (z(dofInd2) < -eps)
               {
                  y(dofInd) += (1. - alpha(j)) * weightN * z(dofInd2); // TODO do not use if
               }
            }
            if (fct.monoType == ResDist_LimMass)
            {
               y(dofInd) += alpha(i) * z(dofInd);
            }
            else
            {
               y(dofInd) = (y(dofInd) + alpha(i) * z(dofInd)) / fct.lumpedM(dofInd);
            }
         }
      }
   }
}

void FE_Evolution::ComputeHighOrderSolution(const Vector &x, Vector &y) const
{
   // No monotonicity treatment, straightforward high-order scheme
   // ydot = M^{-1} (K x + b)
   K.Mult(x, z);
   z += b;
   M_solver.Mult(z, y);
}

void FE_Evolution::ComputeFCTSolution(const Vector &x, const Vector &yH,
                                      const Vector &yL, Vector &y) const
{
   // High order reconstruction that yields an updated admissible solution by means of
   // clipping the solution coefficients within certain bounds and scaling the anti-
   // diffusive fluxes in a way that leads to local conservation of mass.
   int j, k, nd, dofInd;
   double sumPos, sumNeg, eps = 1.E-15;
   Vector uClipped, fClipped;

   // Monotonicity terms
   for (k = 0; k < fes->GetMesh()->GetNE(); k++)
   {
      const FiniteElement &el = *fes->GetFE(k);
      nd = el.GetDof();

      uClipped.SetSize(nd); uClipped = 0.;
      fClipped.SetSize(nd); fClipped = 0.;
      sumPos = sumNeg = 0.;
      for (j = 0; j < nd; j++)
      {
         dofInd = k*nd+j;
         uClipped(j) = min(fct.bnds.x_max(dofInd), max(x(dofInd) + dt * yH(dofInd),
                                                       fct.bnds.x_min(dofInd)));
         // compute coefficients for the high-order corrections
         fClipped(j) = fct.lumpedM(dofInd) * (uClipped(j) - ( x(dofInd) + dt * yL(dofInd) ));

         sumPos += max(fClipped(j), 0.);
         sumNeg += min(fClipped(j), 0.);
      }

      for (j = 0; j < nd; j++)
      {
         if ((sumPos + sumNeg > eps) && (fClipped(j) > eps))
         {
            fClipped(j) *= - sumNeg / sumPos;
         }
         if ((sumPos + sumNeg < -eps) && (fClipped(j) < -eps))
         {
            fClipped(j) *= - sumPos / sumNeg;
         }

         dofInd = k*nd+j;
         // yH is high order discrete time derivative
         // yL is low order discrete time derivative
         y(dofInd) = yL(dofInd) + fClipped(j) / (dt * fct.lumpedM(dofInd));
         // y is now the discrete time derivative featuring the high order anti-diffusive
         // reconstruction that leads to an forward Euler updated admissible solution.
         // The factor dt in the denominator is used for compensation in the ODE solver.
      }
   }
}

void FE_Evolution::ComputeLimitedSolution(const Vector &x, Vector &y) const
{
   int i, j, k, nd, dofInd;
   double zMax, zMin, beta = 0.5, eps = 1.E-15;
   Vector alpha;
   DenseMatrix Mlim;
   Array<int> loc;
   
   int* I = M.GetI();
   int* J = M.GetJ();
   double* Mij = M.GetData();
   int ctr = 0;
   
   M_solver.Mult(y, z);
   
   for (k = 0; k < fes->GetMesh()->GetNE(); k++)
   {
      const FiniteElement &el = *fes->GetFE(k);
      nd = el.GetDof();
      
      alpha.SetSize(nd); alpha = 1.;
      zMin = numeric_limits<double>::infinity();
      zMax = -zMin;
      for (i = 0; i < nd; i++)
      {
         dofInd = k*nd+i;
         zMax = max(zMax, z(dofInd));
         zMin = min(zMin, z(dofInd));
      }
      for (i = 0; i < nd; i++)
      {
         dofInd = k*nd+i;
         alpha(i) = min( 1., beta / dt * min(fct.bnds.x_max(dofInd) - x(dofInd), x(dofInd) - fct.bnds.x_min(dofInd)) 
                                      / (max(zMax - z(dofInd), z(dofInd) - zMin) + eps) ); // TODO repeat of numerator
//          if ((alpha(i) > 1.+eps) || (alpha(i) < -eps))
//             mfem_error(".");
      }
      for (i = 0; i < nd; i++)
      {
         dofInd = k*nd+i;
         for (j = nd-1; j >= 0; j--) // run backwards through columns
         {
            if (i==j) { ctr++; continue; }
            
            y(dofInd) += alpha(i) * Mij[ctr] * alpha(j) * (z(dofInd) - z(k*nd+j)); // use knowledge of how M looks like
            ctr++;
         }
         y(dofInd) /= fct.lumpedM(dofInd);
      }
   }
}

// Implementation of class FE_Evolution
FE_Evolution::FE_Evolution(FiniteElementSpace* _fes, SparseMatrix &_M,
                           SparseMatrix &_K,
                           const Vector &_b, FluxCorrectedTransport &_fct)
   : TimeDependentOperator(_M.Size()), fes(_fes), M(_M), K(_K), b(_b),
     z(_M.Size()), fct(_fct)
{
   M_solver.SetPreconditioner(M_prec);
   M_solver.SetOperator(M);

   M_solver.iterative_mode = false;
   M_solver.SetRelTol(1e-9);
   M_solver.SetAbsTol(0.0);
   M_solver.SetMaxIter(200);
   M_solver.SetPrintLevel(0);
}

void FE_Evolution::Mult(const Vector &x, Vector &y) const
{
   if (fct.monoType == 0)
   {
      ComputeHighOrderSolution(x, y);
   }
   else if (fct.monoType < 7)
   {
      if (fct.monoType % 2 == 1)
      {
         ComputeLowOrderSolution(x, y);
      }
      else if (fct.monoType % 2 == 0)
      {
         Vector yH, yL;
         yH.SetSize(x.Size()); yL.SetSize(x.Size());
         
         ComputeHighOrderSolution(x, yH);
         ComputeLowOrderSolution(x, yL);
         ComputeFCTSolution(x, yH, yL, y);
      }
   }
   else if (fct.monoType == 7)
   {
      ComputeLowOrderSolution(x, y);
   }
   else if (fct.monoType == 8)
   {
      ComputeLowOrderSolution(x, y);
      ComputeLimitedSolution(x, y);
   }
}


// Velocity coefficient
void velocity_function(const Vector &x, Vector &v)
{
   int dim = x.Size();

   // map to the reference [-1,1] domain
   Vector X(dim);
   for (int i = 0; i < dim; i++)
   {
      double center = (bb_min[i] + bb_max[i]) * 0.5;
      X(i) = 2 * (x(i) - center) / (bb_max[i] - bb_min[i]);
   }

   switch (problem)
   {
      case 0:
      {
         // Translations in 1D, 2D, and 3D
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = sqrt(2./3.); v(1) = sqrt(1./3.); break;
            case 3: v(0) = sqrt(3./6.); v(1) = sqrt(2./6.); v(2) = sqrt(1./6.);
               break;
         }
         break;
      }
      case 1:
      case 2:
      case 4:
      {
         // Clockwise rotation in 2D around the origin
         const double w = M_PI/2;
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = w*X(1); v(1) = -w*X(0); break;
            case 3: v(0) = w*X(1); v(1) = -w*X(0); v(2) = 0.0; break;
         }
         break;
      }
      case 3:
      {
         // Clockwise twisting rotation in 2D around the origin
         const double w = M_PI/2;
         double d = max((X(0)+1.)*(1.-X(0)),0.) * max((X(1)+1.)*(1.-X(1)),0.);
         d = d*d;
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = d*w*X(1); v(1) = -d*w*X(0); break;
            case 3: v(0) = d*w*X(1); v(1) = -d*w*X(0); v(2) = 0.0; break;
         }
         break;
      }
      case 5:
      {
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = 1.0; v(1) = 1.0; break;
            case 3: v(0) = 1.0; v(1) = 1.0; v(2) = 1.0; break;
         }
      }
   }
}

bool insidePolygon(double xpoints[12], double ypoints[12], double x, double y)
{
  bool oddNodes = false;
  int j = 12; // TODO fixed size
  for (int i = 0; i < 12; i++)
  {
    if ((ypoints[i] < y) && (ypoints[j] >= y) || (ypoints[j] < y) || (ypoints[i] >= y))
    {
      if (xpoints[i] + ( y - ypoints[i] ) / (ypoints[j] - ypoints[i]) * (xpoints[j] - xpoints[i]) < x)
      {
        oddNodes = !oddNodes;
      }
    }
    j = i;
  }
   return oddNodes;
}

bool ball(double x0, double y0, double r, double x, double y)
{
  double xr = x-x0;
  double yr = y-y0;
  double rsq = xr*xr + yr*yr;

  if (rsq < r*r) { return true; }
  return false;
}

// Initial condition
double u0_function(const Vector &x)
{
   int dim = x.Size();

   // map to the reference [-1,1] domain
   Vector X(dim);
   for (int i = 0; i < dim; i++)
   {
      double center = (bb_min[i] + bb_max[i]) * 0.5;
      X(i) = 2 * (x(i) - center) / (bb_max[i] - bb_min[i]);
   }

   switch (problem)
   {
      case 0:
      case 1:
      {
         switch (dim)
         {
            case 1:
               return exp(-40.*pow(X(0)-0.5,2));
            case 2:
            case 3:
            {
               double rx = 0.45, ry = 0.25, cx = 0., cy = -0.2, w = 10.;
               if (dim == 3)
               {
                  const double s = (1. + 0.25*cos(2*M_PI*X(2)));
                  rx *= s;
                  ry *= s;
               }
               return ( erfc(w*(X(0)-cx-rx))*erfc(-w*(X(0)-cx+rx)) *
                        erfc(w*(X(1)-cy-ry))*erfc(-w*(X(1)-cy+ry)) )/16;
            }
         }
      }
      case 2:
      {
         double x_ = X(0), y_ = X(1), rho, phi;
         rho = hypot(x_, y_);
         phi = atan2(y_, x_);
         return pow(sin(M_PI*rho),2)*sin(3*phi);
      }
      case 3:
      {
         const double f = M_PI;
         return .5*(sin(f*X(0))*sin(f*X(1)) + 1.); // modified by Hennes
      }
      case 4:
      {
         double scale = 0.09;
         double slit = (X(0) <= -0.05) || (X(0) >= 0.05) || (X(1) >= 0.7);
         double cone = (1./sqrt(scale)) * sqrt(pow(X(0), 2.) + pow(X(1) + 0.5,2.));
         double bump = (1./sqrt(scale)) * sqrt(pow(X(0) - 0.5,2.) + pow(X(1), 2.));

         return (slit && ((pow(X(0),2.) + pow(X(1) - 0.5,2.)) <= scale)) ? 1. : 0.
                + (1-cone) * (pow(X(0),2.) + pow(X(1) + 0.5,2.) <= scale)
                + 0.25*(1.+cos(M_PI*bump))*((pow(X(0) - 0.5,2.) + pow(X(1),2.)) <= scale);
      }
      case 5:
      {
         if (ball(0.4, 0.4, 0.07, X(0), X(1))) { return 2.0; }
         if (ball(0.4, 0.4, 0.10, X(0), X(1))) { return 1.0; }
         
         if (ball(0.4, 0.2, 0.03, X(0),X(1))) { return 3.0; }
         if (ball(0.4, 0.2, 0.07, X(0),X(1))) { return 2.0; }
         if (ball(0.4, 0.2, 0.10, X(0),X(1))) { return 1.0; }

         // straight cross
//          double xcross1[12] = {.270, .270, .120, .120, .090, .090, .020, .020, .090, .090, .120, .120};
//          double ycross1[12] = {.300, .330, .330, .460, .460, .330, .330, .300, .300, .230, .230, .300};
//          if (insidePolygon(xcross1, ycross1, X(0), X(1))) { return 1.0; }
//          
         // diagonal cross
//          double xcross2[12] = {.0200000, .0624264, .0800000, .0975736, .1400000, .1012131, .2224264, .1800000, .0800000, .0200000, .0200000, .0587868};
//          double ycross2[12] = {.030000, .030000, .0475736, .0300000, .0300000, .0687868, .190000, .190000, .0900000, .1500000, .1075736, .0687868};
//          if (insidePolygon(xcross2, ycross2, X(0), X(1))) { return 1.0; }

         return 0.;
      }
   }
   return 0.0;
}

// Inflow boundary condition (zero for the problems considered in this example)
double inflow_function(const Vector &x)
{
   switch (problem)
   {
      case 0:
      case 1:
      case 2:
      case 3:
      case 4: return 0.0;
   }
   return 0.0;
}
