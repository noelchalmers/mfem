#include "DofMapsDST.hpp"


double testcoeff(const Vector & x)
{
   // return x(0)*x(0)+x(1)*x(1);
   return sin(3*M_PI*(x(0)+x(1)));
}

int get_rank(int tdof, std::vector<int> & tdof_offsets)
{
   int size = tdof_offsets.size();
   if (size == 1) { return 0; }
   std::vector<int>::iterator up;
   up=std::upper_bound(tdof_offsets.begin(), tdof_offsets.end(),tdof); // 
   return std::distance(tdof_offsets.begin(),up)-1;
}

void ComputeTdofOffsets(const MPI_Comm & comm, const ParFiniteElementSpace * pfes, 
                        std::vector<int> & tdof_offsets)
{
   int num_procs;
   MPI_Comm_size(comm, &num_procs);
   tdof_offsets.resize(num_procs);
   int mytoffset = pfes->GetMyTDofOffset();
   MPI_Allgather(&mytoffset,1,MPI_INT,&tdof_offsets[0],1,MPI_INT,comm);
}

void GetSubdomainijk(int ip, const Array<int> nxyz, Array<int> & ijk)
{
   ijk.SetSize(3);
   ijk[2] = ip/(nxyz[0]*nxyz[1]);
   ijk[1] = (ip-ijk[2]*nxyz[0]*nxyz[1])/nxyz[0];
   ijk[0] = (ip-ijk[2]*nxyz[0]*nxyz[1])%nxyz[0];
}
void GetDirectionijk(int id, Array<int> & ijk)
{
   ijk.SetSize(3);
   int n = 3;
   ijk[2] = id/(n*n) - 1;
   ijk[1] = (id-(ijk[2]+1)*n*n)/n - 1;
   ijk[0] = (id-(ijk[2]+1)*n*n)%n - 1;
}

int GetSubdomainId(const Array<int> nxyz, Array<int> & ijk)
{
   int dim=ijk.Size();
   int k = (dim==2)? 0 : ijk[2];
   return k*nxyz[1]*nxyz[0] + ijk[1]*nxyz[0] + ijk[0];
}

int GetDirectionId(const Array<int> & ijk)
{
   int n = 3;
   int dim = ijk.Size();
   int k = (dim == 2) ? -1 : ijk[2];
   return (k+1)*n*n + (ijk[1]+1)*n + ijk[0]+1; 
}

void DofMaps::Init()
{
   comm = pfes->GetComm();
   MPI_Comm_size(comm, &num_procs);
   MPI_Comm_rank(comm, &myid);

   dim = pfes->GetParMesh()->Dimension();
   ComputeTdofOffsets(comm, pfes, tdof_offsets);
   myelemoffset = part->myelem_offset;
   mytoffset = pfes->GetMyTDofOffset();
   subdomain_rank = part->subdomain_rank;
   nrsubdomains = part->nrsubdomains;
   nxyz.SetSize(3);
   for (int i = 0; i<3; i++) { nxyz[i] = part->nxyz[i]; }
}

DofMaps::DofMaps(ParFiniteElementSpace *pfes_, ParMeshPartition * part_)
: pfes(pfes_), part(part_)
{
   Init();
   Setup();
}

void DofMaps::Setup()
{
   // Setup the local FiniteElementSpaces
   const FiniteElementCollection * fec = pfes->FEColl();
   fes.SetSize(nrsubdomains);
   for (int i = 0; i<nrsubdomains; i++)
   {
      fes[i] = nullptr; // initialize with null on all procs
      if (myid == subdomain_rank[i])
      {
         fes[i] = new FiniteElementSpace(part->subdomain_mesh[i],fec);
      }
   }
   // cout << "Computing Overlap Tdofs" << endl;
   SubdomainToSubdomainMapsSetup();
   // TestSubdomainToSubdomainMaps();

   SubdomainToGlobalMapsSetup();
   TestSubdomainToGlobalMaps();


}

void DofMaps::SubdomainToSubdomainMapsSetup()
{
   ComputeOvlpElems();
   ComputeOvlpTdofs();
}

void DofMaps::AddElementToOvlpLists(int l, int iel, 
                            const Array<bool> & neg, const Array<bool> & pos)
{
   int kbeg = (dim == 2) ? 0 : -1;
   int kend = (dim == 2) ? 0 :  1;
   Array<int> dijk(3);
   for (int k = kbeg; k<=kend; k++)
   {
      if (dim == 3)
      {
         if (k == -1 && !neg[2]) continue;   
         if (k ==  1 && !pos[2]) continue;   
      }   

      for (int j = -1; j<=1; j++)
      {
         if (j== -1 && !neg[1]) continue;   
         if (j==  1 && !pos[1]) continue;   
         for (int i = -1; i<=1; i++)
         {
            // cases to skip 
            if (i==-1 && !neg[0]) continue;   
            if (i== 1 && !pos[0]) continue;   

            if (i==0 && j==0 && k == 0) continue;
            dijk[0] = i; dijk[1] = j; dijk[2] = (dim==2)?-1 : k;  
            int DirId = GetDirectionId(dijk);
            OvlpElems[l][DirId].Append(iel);
         }
      }
   }
}

void DofMaps::ComputeOvlpElems()
{
   // first compute the element in the overlaps
   OvlpElems.resize(nrsubdomains);
   int nlayers = 2*part->OvlpNlayers; 
   // loop through subdomains
   for (int l = 0; l<nrsubdomains; l++)
   {
      if (myid == subdomain_rank[l])
      {
         Array<int> ijk;
         GetSubdomainijk(l,nxyz,ijk);
         Mesh * mesh = part->subdomain_mesh[l];
         OvlpElems[l].resize(pow(3,dim)); 
         Vector pmin, pmax;
         mesh->GetBoundingBox(pmin,pmax);
         double h = part->MeshSize;
         // loop through the elements in the mesh and assign them to the 
         // appropriate lists of overlaps
         for (int iel=0; iel< mesh->GetNE(); iel++)
         {
            // Get element center
            Vector center(dim);
            int geom = mesh->GetElementBaseGeometry(iel);
            ElementTransformation * tr = mesh->GetElementTransformation(iel);
            tr->Transform(Geometries.GetCenter(geom),center);

            Array<bool> pos(dim); pos = false;
            Array<bool> neg(dim); neg = false;
            // loop through dimensions   
            for (int d=0;d<dim; d++)
            {
               if (ijk[d]>0 && center[d] < pmin[d]+h*nlayers)
               {
                  neg[d] = true;
               }

               if (ijk[d]<nxyz[d]-1 && center[d] > pmax[d]-h*nlayers) 
               {
                  pos[d] = true;
               }
            }
            // Add the element to the appropriate lists
            AddElementToOvlpLists(l,iel,neg,pos);        
         }   
      }
   }
}

void DofMaps::ComputeOvlpTdofs()
{

   OvlpTDofs.resize(nrsubdomains);
   OvlpSol.resize(nrsubdomains);
   int nrneighbors = pow(3,dim); // including its self

   // loop through subdomains
   cout << "nrsubdomains = " << nrsubdomains << endl;
   for (int l = 0; l<nrsubdomains; l++)
   {
      if (myid == subdomain_rank[l])
      {
         int ntdofs = fes[l]->GetTrueVSize();
         Array<int> tdof_marker(ntdofs); 
         OvlpTDofs[l].resize(nrneighbors);
         OvlpSol[l].resize(nrneighbors);
         // loop through neighboring directions/neighbors
         for (int d=0; d<nrneighbors; d++)
         {
            OvlpSol[l][d] = nullptr;
            tdof_marker = 0;
            Array<int> tdoflist;
            // Get the direction
            Array<int> dijk;
            GetDirectionijk(l,dijk);
            int nel = OvlpElems[l][d].Size();
            Array<int>Elems = OvlpElems[l][d];
            for (int iel = 0; iel<nel; ++iel)
            {
               int jel = Elems[iel];   
               Array<int> ElemDofs;

               fes[l]->GetElementDofs(jel,ElemDofs);
               int ndof = ElemDofs.Size();
               for (int i = 0; i<ndof; ++i)
               {
                  int dof_ = ElemDofs[i];
                  int dof = (dof_ >= 0) ? dof_ : abs(dof_) - 1;
                  if (!tdof_marker[dof])
                  {
                     tdoflist.Append(dof); // dofs of ip0 in ovlp
                     tdof_marker[dof] = 1;
                  }
               }
            }
            OvlpTDofs[l][d] = tdoflist; 
         }
      }
   }
}

void DofMaps::PrintOvlpTdofs()
{
   int nrneighbors = pow(3,dim); // including its self
   if (myid == 0)
   {
      for (int i = 0; i<nrsubdomains; i++)
      {
         if (myid == subdomain_rank[i])
         {
            Array<int> ijk;
            GetSubdomainijk(i,nxyz,ijk);
            cout << "subdomain = " ; ijk.Print();
            cout << "myid = " << myid << endl;
            cout << "ip   = " << i << endl;
            for (int d = 0; d<nrneighbors; d++)
            {
               Array<int> dijk;
               GetDirectionijk(d,dijk);
               cout << "direction = " ; dijk.Print();

               if (OvlpTDofs[i][d].Size())
               {
                  cout << "OvlpTdofs = " ; 
                  OvlpTDofs[i][d].Print(cout,OvlpTDofs[i][d].Size() );
               }
            }
         }
      }
   }
}

void DofMaps::TransferToNeighbors(const Array<int> & SubdomainIds, const Array<Vector *> & x)
{
   // 2D for now....
   MFEM_VERIFY(SubdomainIds.Size() == x.Size(), "TransferToNeighbors: Size inconsistency");
   int nrsendIds = SubdomainIds.Size();
   int nrneighbors = pow(3,dim);
   MPI_Request *recv_requests = new MPI_Request[nrsendIds*nrneighbors];
   MPI_Request *send_requests = new MPI_Request[nrsendIds*nrneighbors];
   MPI_Status  *recv_statuses = new MPI_Status[nrsendIds*nrneighbors];
   MPI_Status  *send_statuses = new MPI_Status[nrsendIds*nrneighbors];
   Array<Vector * > send_buffer(nrsendIds*nrneighbors);
   Array<Vector * > recv_buffer(nrsendIds*nrneighbors);
   int send_counter = 0;
   int recv_counter = 0;
   for (int is = 0; is<nrsendIds; is++)
   {
      int i0 = SubdomainIds[is];
      Array<int> ijk;
      GetSubdomainijk(i0,nxyz,ijk);
      for (int d=0;d<nrneighbors; d++)
      {
         Array<int>directions;
         GetDirectionijk(d,directions);

         if (dim == 2 && directions[0] == 0 && directions[1] == 0) continue;
         if (dim == 3 && directions[0] == 0 
                      && directions[1] == 0 
                      && directions[2] == 0) continue;
         int i = ijk[0] + directions[0];
         if (i<0 || i>=nxyz[0]) continue;
         int j = ijk[1] + directions[1];
         if (j<0 || j>=nxyz[1]) continue;
         int k = (dim ==3 ) ? ijk[2] + directions[2] : 0;
         if (k<0 || k>=nxyz[2]) continue;
         Array<int>ijk1(3);
         ijk1[0] = i;
         ijk1[1] = j;
         ijk1[2] = k;
         int i1 = GetSubdomainId(nxyz,ijk1);
         if (myid == subdomain_rank[i0])
         {
            Array<int> tdofs0 = OvlpTDofs[i0][d]; // map of dofs in the overlap
            send_buffer[send_counter] = new Vector(tdofs0.Size());
            x[i0]->GetSubVector(tdofs0,*send_buffer[send_counter]);
            // Destination rank
            int dest = subdomain_rank[i1];
            // cout << "sending from (id,sub) = (" << myid << "," << i0 << ") to: " 
               //   << "(" << dest << ","<< i1 << ")" << endl;
            int tag = i0 * nrneighbors + d;

            int count = tdofs0.Size();
            MPI_Isend(send_buffer[send_counter]->GetData(),count,MPI_DOUBLE,dest,
                      tag,comm,&send_requests[send_counter]);
            send_counter++;
         }
         if (myid == subdomain_rank[i1])
         {
            Array<int> direction1(3); direction1 = -1;
            for (int d=0;d<dim;d++) 
            { 
               direction1[d] = -directions[d];
            }
            int d1 = GetDirectionId(direction1);

            int count = OvlpTDofs[i1][d1].Size();
            recv_buffer[recv_counter] = new Vector(count); 
            int src = subdomain_rank[i0];
            int tag = i0 * nrneighbors + d;
            MPI_Irecv(recv_buffer[recv_counter]->GetData(), count,MPI_DOUBLE,src,
                      tag,comm, &recv_requests[recv_counter]);
            recv_counter++;
         }
      }   
   }
   MPI_Waitall(send_counter, send_requests, send_statuses);
   MPI_Waitall(recv_counter, recv_requests, recv_statuses);

   delete [] send_statuses;
   delete [] send_requests;
   delete [] recv_statuses;
   delete [] recv_requests;

   for (int i = 0; i<send_counter; i++)
   {
      delete send_buffer[i];
   }
   send_buffer.DeleteAll();


   // Extruct the transfered solutions
   recv_counter = 0;
   for (int is = 0; is<nrsendIds; is++)
   {
      int i0 = SubdomainIds[is];
      Array<int> ijk;
      GetSubdomainijk(i0,nxyz,ijk);
      for (int d=0;d<nrneighbors; d++)
      {
         Array<int>directions;
         GetDirectionijk(d,directions);
         if (dim == 2 && directions[0] == 0 && directions[1] == 0) continue;
         if (dim == 3 && directions[0] == 0 
                      && directions[1] == 0 
                      && directions[2] == 0) continue;
         int i = ijk[0] + directions[0];
         if (i<0 || i>=nxyz[0]) continue;
         int j = ijk[1] + directions[1];
         if (j<0 || j>=nxyz[1]) continue;
         int k = (dim ==3 ) ? ijk[2] + directions[2] : 0;
         if (k<0 || k>=nxyz[2]) continue;

         Array<int>ijk1(3);
         ijk1[0] = i;
         ijk1[1] = j;
         ijk1[2] = k;
         int i1 = GetSubdomainId(nxyz,ijk1);
         if (myid == subdomain_rank[i1])
         {
            Array<int> direction1(3); direction1 = -1;
            for (int d=0;d<dim;d++) 
            { 
               direction1[d] = -directions[d];
            }
            int d1 = GetDirectionId(direction1);
            Array<int> tdofs1 = OvlpTDofs[i1][d1];
            OvlpSol[i1][d1] = new Vector(fes[i1]->GetTrueVSize());
            *OvlpSol[i1][d1] = 0.0;
            OvlpSol[i1][d1]->SetSubVector(tdofs1,*recv_buffer[recv_counter]);
            recv_counter++;
         }
      }   
   }
   for (int i = 0; i<recv_counter; i++)
   {
      delete recv_buffer[i];
   }
   recv_buffer.DeleteAll();
}

void DofMaps::TestSubdomainToSubdomainMaps()
{
      // testing inter-subdomain communication
   FunctionCoefficient c1(testcoeff);
   int nrsub = nrsubdomains;
   Array<int> subdomain_ids(nrsub);
   Array<Vector*> x(nrsub);
   for (int i = 0; i<nrsub; i++)
   {
      x[i] = nullptr;
      subdomain_ids[i] = i;
      if (fes[i])
      {
         GridFunction gf(fes[i]);
         gf = 0.0;
         gf.ProjectCoefficient(c1);
         x[i] = new Vector(fes[i]->GetTrueVSize());
         *x[i] = gf;
      }
   }

   TransferToNeighbors(subdomain_ids,x);
   int nrneighbors = pow(3,dim);

   string keys = "keys amrRljc\n";
   for (int i0 = 0 ; i0< nrsubdomains; i0++)
   {
      if (fes[i0])
      {
         GridFunction gf0(fes[i0]);
         for (int d = 0; d<nrneighbors; d++)
         {

            if(OvlpSol[i0][d])
            {
               Array<int>dijk;
               GetDirectionijk(d,dijk);
               Array<int>ijk;
               GetSubdomainijk(i0,nxyz,ijk);
               ostringstream oss;
               oss << "myid: " << myid 
                     << ", subdomain: (" << ijk[0] << "," << ijk[1] <<")"
                     << ", direction: (" << dijk[0] << "," << dijk[1] <<")";

               gf0 = 0.0;
               gf0.SetVector(*OvlpSol[i0][d],0);
               char vishost[] = "localhost";
               int  visport   = 19916;
               socketstream sol_sock(vishost, visport);
               sol_sock.precision(8);
               sol_sock << "solution\n" << *(part->subdomain_mesh[i0]) << gf0 
               << keys
               << "window_title '" << oss.str() << "'" << flush;
            }
         }
      }
   }
   for (int i = 0; i<nrsub; i++)
   {
      delete x[i];
   }
}

void DofMaps::SubdomainToGlobalMapsSetup()
{
   // workspace for MPI_AlltoAll
   send_count.SetSize(num_procs);  send_count = 0;
   send_displ.SetSize(num_procs);  send_displ = 0; 
   recv_count.SetSize(num_procs);  recv_count = 0;
   recv_displ.SetSize(num_procs);  recv_displ = 0;

   // 1. Construct a list of local (to the processor) tdofs 
   //    of the global mesh for each subdomain
   std::vector<Array<int>> local_tdofs(nrsubdomains);
   // Array<int> dof_marker(pfes->GetTrueVSize());
   Array<int> dof_marker(pfes->GetVSize());
   for (int ip = 0; ip<nrsubdomains; ++ip)
   {
      dof_marker = 0;
      int nel = part->local_element_map[ip].Size();
      for (int iel = 0; iel<nel; iel++)
      {
         int elem_idx = part->local_element_map[ip][iel] - myelemoffset;
         Array<int> element_dofs;
         pfes->GetElementDofs(elem_idx, element_dofs);
         int ndofs = element_dofs.Size();
         for (int i=0; i<ndofs; i++)
         {
            int edof_ = element_dofs[i];
            int edof = (edof_ >= 0) ? edof_ : abs(edof_) - 1;
            int tdof = pfes->GetGlobalTDofNumber(edof);
            // if (myid != get_rank(tdof,tdof_offsets)) continue;
            // if (dof_marker[tdof-mytoffset]) continue;
            // if (dof_marker[edof]) continue;
            local_tdofs[ip].Append(tdof);
            // dof_marker[edof] = 1;
            // dof_marker[tdof-mytoffset] = 1;
         }
      }
   }


   // The above does not take care of tdofs on neighboring processor 
   // that doesn't own the element

   // 2. Communicate to the host rank
   // a. Compute send count
   for (int ip = 0; ip<nrsubdomains; ++ip)
   {
      int ndofs = local_tdofs[ip].Size();
      if (ndofs)
      {
         // Data to send to the subdomain rank
         // 1. Subdomain no 
         // 2. Number of dofs
         // 3. The list of dofs
         send_count[subdomain_rank[ip]] += 2 + ndofs; 
      }
   }
   // b. Compute receive count
   MPI_Alltoall(send_count,1,MPI_INT,recv_count,1,MPI_INT,comm);
   for (int k=0; k<num_procs-1; k++)
   {
      send_displ[k+1] = send_displ[k] + send_count[k];
      recv_displ[k+1] = recv_displ[k] + recv_count[k];
   }
   sbuff_size = send_count.Sum();
   rbuff_size = recv_count.Sum();
   // c. Allocate and fill the send buffer
   Array<int> sendbuf(sbuff_size);  sendbuf = 0;
   Array<int> soffs(num_procs); soffs = 0;
   for (int ip = 0; ip<nrsubdomains; ++ip)
   {
      int ndofs = local_tdofs[ip].Size();
      if (ndofs)
      {
         // Data to send to the subdomain rank
         // 1. Subdomain no 
         // 2. Number of dofs
         // 3. The list of dofs
         int j = send_displ[subdomain_rank[ip]] + soffs[subdomain_rank[ip]];
         sendbuf[j] = ip;
         sendbuf[j+1] = ndofs;
         for (int k = 0; k < ndofs ; ++k)
         {
            sendbuf[j+2+k] = local_tdofs[ip][k];
         }
         soffs[subdomain_rank[ip]] +=  2 + ndofs;
      }
   }

   // d. Communication
   Array<int> recvbuf(rbuff_size);
   MPI_Alltoallv(sendbuf, send_count, send_displ, MPI_INT, recvbuf,
                 recv_count, recv_displ, MPI_INT, comm);

   // 3. Extract from recv_buffer
   std::vector<Array<int>> global_tdofs(nrsubdomains);
   int k=0;
   while (k<rbuff_size)
   {
      int ip = recvbuf[k++]; 
      int ndofs = recvbuf[k++];
      for (int i = 0; i < ndofs; ++i)
      {
         global_tdofs[ip].Append(recvbuf[i+k]); 
      }
      k += ndofs;
   }

   // local_tdofs[2].Print();
   
   

   SubdomainGTrueDofs.resize(nrsubdomains);
   // 4. Construct SubdomainTdof to Global mesh tdof maps
   for (int ip=0; ip<nrsubdomains; ++ip)
   {
      if (myid != subdomain_rank[ip]) continue;
      int nrdof = fes[ip]->GetTrueVSize();

      // if (ip==2 && nrdof != global_tdofs[ip].Size())
      // {
      //    cout << "nrdof = " << nrdof << endl;
      //    cout << "global_tdofs[ip].Size() = " << global_tdofs[ip].Size() << endl;
      //    cout << "ip = " << ip << endl;
      //    global_tdofs[ip].Print();
      // }

      // MFEM_VERIFY(nrdof == global_tdofs[ip].Size(),"TrueDof list size inconsistency");
      Array<int> dof_marker(nrdof); dof_marker = 0;
      SubdomainGTrueDofs[ip].SetSize(nrdof);
      int nel = part->element_map[ip].Size();
      int k = 0;
      for (int iel = 0; iel<nel; ++iel)
      {
         Array<int> elem_dofs;
         fes[ip]->GetElementDofs(iel,elem_dofs);
         int ndof = elem_dofs.Size();
         for (int i = 0; i<ndof; ++i)
         {
            int edof_ = elem_dofs[i];
            int edof = (edof_ >= 0) ? edof_ : abs(edof_) - 1;
            // if (dof_marker[edof]) continue;
            // rearranging dofs from serial fespace for the subdomain 
            // to the ordering of the pfespace
            SubdomainGTrueDofs[ip][edof] = global_tdofs[ip][k];
            k++;
            // dof_marker[edof] = 1;
         }
      }
   }

   // 5. Communicate SubdomainGTrueDofs to participating ranks 
   send_count = 0;
   send_displ = 0;
   recv_count = 0;
   recv_displ = 0;

   for (int ip = 0; ip < nrsubdomains; ++ip)
   {
      if (myid != subdomain_rank[ip]) continue;
      Array<int> ranks_marker(num_procs); ranks_marker = 0;

      int ndofs = SubdomainGTrueDofs[ip].Size();
      for (int i = 0; i<ndofs; ++i)
      {
         int tdof = SubdomainGTrueDofs[ip][i];
         int rank = get_rank(tdof,tdof_offsets);
         if (!ranks_marker[rank]) { ranks_marker[rank] = 1; }
      }
      for (int irank = 0; irank<num_procs; ++irank)
      {
         if (ranks_marker[irank]) { send_count[irank] += 2+ndofs; } // patch_number and size
      }
   }

   // communicate so that recv_count is constructed
   MPI_Alltoall(send_count,1,MPI_INT,recv_count,1,MPI_INT,comm);
   //
   for (int k=0; k<num_procs-1; k++)
   {
      send_displ[k+1] = send_displ[k] + send_count[k];
      recv_displ[k+1] = recv_displ[k] + recv_count[k];
   }
   sbuff_size = send_count.Sum();
   rbuff_size = recv_count.Sum();

   sendbuf.SetSize(sbuff_size);  sendbuf = 0;
   soffs = 0;

   for (int ip = 0; ip < nrsubdomains; ip++)
   {
      if (myid != subdomain_rank[ip]) continue;
      int ndofs = SubdomainGTrueDofs[ip].Size();
         // loop through dofs
      Array<int> ranks_marker(num_procs); ranks_marker = 0;
      // loop through the dofs and find their rank
      for (int i = 0; i<ndofs; ++i)
      {
         int tdof = SubdomainGTrueDofs[ip][i];
         int rank = get_rank(tdof,tdof_offsets);
         if (!ranks_marker[rank]) { ranks_marker[rank] = 1; }
      }
      for (int irank = 0; irank<num_procs; ++irank)
      {
         if (ranks_marker[irank])
         {
            int j = send_displ[irank] + soffs[irank];
            sendbuf[j] = ip;
            sendbuf[j+1] = ndofs;
            for (int i = 0; i<ndofs; ++i)
            {
               sendbuf[j+2+i] = SubdomainGTrueDofs[ip][i];
            }
            soffs[irank] += 2 + ndofs ;
         }
      }
   }

   recvbuf.SetSize(rbuff_size);
   MPI_Alltoallv(sendbuf, send_count, send_displ, MPI_INT, recvbuf,
                 recv_count, recv_displ, MPI_INT, comm);

   // Construct local tdof lists for each patch
   SubdomainLTrueDofs.resize(nrsubdomains);

   k=0;
   while (k<rbuff_size)
   {
      int ip = recvbuf[k++];
      int ndofs = recvbuf[k++];
      for (int idof = 0; idof < ndofs; ++idof)
      {
         int tdof = recvbuf[k+idof];
         if (myid != subdomain_rank[ip]) { SubdomainGTrueDofs[ip].Append(tdof); }
         if (get_rank(tdof,tdof_offsets) == myid)
         {
            SubdomainLTrueDofs[ip].Append(tdof);
         }
      }
      k += ndofs;
   }               
   // cout<< "LT " ; SubdomainLTrueDofs[0].Print();
   // cout<< "GT " ;SubdomainGTrueDofs[0].Print();
}

   

   // Restriction of global residual to subdomain residuals
void DofMaps::GlobalToSubdomains(const Vector & y, std::vector<Vector> & x)
{
   send_count = 0;
   send_displ = 0;
   recv_count = 0;
   recv_displ = 0;

   // Compute send_counts
   for (int ip = 0; ip < nrsubdomains; ip++)
   {
      int ndofs = SubdomainLTrueDofs[ip].Size();
      for (int i =0; i<ndofs; i++)
      {
         int tdof = SubdomainLTrueDofs[ip][i];
         int tdof_rank = get_rank(tdof,tdof_offsets);
         // if (myid == tdof_rank)
         // {
            send_count[subdomain_rank[ip]]++;
         // }
      }
   }

    // communicate so that recv_count is constructed
   MPI_Alltoall(send_count,1,MPI_INT,recv_count,1,MPI_INT,comm);

   for (int k=0; k<num_procs-1; k++)
   {
      send_displ[k+1] = send_displ[k] + send_count[k];
      recv_displ[k+1] = recv_displ[k] + recv_count[k];
   }
   sbuff_size = send_count.Sum();
   rbuff_size = recv_count.Sum();

   Array<double> sendbuf(sbuff_size); sendbuf = 0;
   Array<int> soffs(num_procs); soffs = 0;

   for (int ip = 0; ip < nrsubdomains; ip++)
   {
      int ndofs = SubdomainLTrueDofs[ip].Size();
      for (int i = 0; i<ndofs; i++)
      {
         int tdof = SubdomainLTrueDofs[ip][i];
         // find its rank
         int tdof_rank = get_rank(tdof,tdof_offsets);
         // if (myid == tdof_rank)
         // {
            int j = send_displ[subdomain_rank[ip]] + soffs[subdomain_rank[ip]];
            soffs[subdomain_rank[ip]]++;
            int k = tdof - mytoffset;
            sendbuf[j] = y[k];
         // }
      }
   }

   // communication
   Array<double> recvbuf(rbuff_size);

   MPI_Alltoallv(sendbuf, send_count, send_displ, MPI_DOUBLE, recvbuf,
                 recv_count, recv_displ, MPI_DOUBLE, comm);
   Array<int> roffs(num_procs);
   roffs = 0;
   // Now each process will construct the res vector
   x.resize(nrsubdomains);
   for (int ip = 0; ip < nrsubdomains; ip++)
   {
      if (myid != subdomain_rank[ip]) continue;
      int ndof = SubdomainGTrueDofs[ip].Size();
      x[ip].SetSize(ndof);
      // extract the data from receiv buffer
      for (int i=0; i<ndof; i++)
      {
         // pick up the tdof and find its rank
         int tdof = SubdomainGTrueDofs[ip][i];
         int tdof_rank = get_rank(tdof,tdof_offsets);
         int k = recv_displ[tdof_rank] + roffs[tdof_rank];
         roffs[tdof_rank]++;
         x[ip][i] = recvbuf[k];
      }
   }
}

// Prolongation of subdomain solutions to the global solution
void DofMaps::SubdomainsToGlobal(const std::vector<Vector> & x, Vector & y)
{
   send_count = 0;
   send_displ = 0;
   recv_count = 0;
   recv_displ = 0;

   // Compute send_counts
   for (int ip = 0; ip < nrsubdomains; ip++)
   {
      if (myid != subdomain_rank[ip]) continue;
      int ndofs = SubdomainGTrueDofs[ip].Size();
      for (int i=0; i<ndofs; i++)
      {
         // pick up the tdof and find its rank
         int tdof = SubdomainGTrueDofs[ip][i];
         int tdof_rank = get_rank(tdof,tdof_offsets);
         send_count[tdof_rank]++;
      }
   }

    // communicate so that recv_count is constructed
   MPI_Alltoall(send_count,1,MPI_INT,recv_count,1,MPI_INT,comm);

   for (int k=0; k<num_procs-1; k++)
   {
      send_displ[k+1] = send_displ[k] + send_count[k];
      recv_displ[k+1] = recv_displ[k] + recv_count[k];
   }
   sbuff_size = send_count.Sum();
   rbuff_size = recv_count.Sum();

   Array<double> sendbuf(sbuff_size); sendbuf = 0;
   Array<int> soffs(num_procs); soffs = 0;

   for (int ip = 0; ip < nrsubdomains; ip++)
   {
      if (myid != subdomain_rank[ip]) continue;
      int ndofs = SubdomainGTrueDofs[ip].Size();
      // loop through dofs
      for (int i=0; i<ndofs; i++)
      {
         //  pick up the dof and find its tdof_rank
         int tdof = SubdomainGTrueDofs[ip][i];
         int tdof_rank = get_rank(tdof,tdof_offsets);
         // offset
         int k = send_displ[tdof_rank] + soffs[tdof_rank];
         soffs[tdof_rank]++;
         sendbuf[k] = x[ip][i];
      }
   }

   Array<double> recvbuf(rbuff_size);
   Array<int> roffs(num_procs); roffs = 0;
   MPI_Alltoallv(sendbuf, send_count, send_displ, MPI_DOUBLE, recvbuf,
                 recv_count, recv_displ, MPI_DOUBLE, comm);

   for (int ip = 0; ip < nrsubdomains; ip++)
   {
      int ndofs = SubdomainLTrueDofs[ip].Size();
      for (int i = 0; i<ndofs; i++)
      {
         int tdof = SubdomainLTrueDofs[ip][i];
         // find its rank
         int k = tdof - mytoffset;
         // int tdof_rank = get_rank(tdof,tdof_offsets);
         // if (myid == tdof_rank)
         // {
            int j = recv_displ[subdomain_rank[ip]] + roffs[subdomain_rank[ip]];
            roffs[subdomain_rank[ip]]++;
            y[k] += recvbuf[j];
         // }
      }
   }              

}



void DofMaps::TestSubdomainToGlobalMaps()
{
   cout << "Testing Subdomain To Global Maps" << endl;
   FunctionCoefficient c1(testcoeff);
   std::vector<Vector> x(nrsubdomains);
   Vector y(pfes->GetTrueVSize()); y = 0.0;
   for (int i = 0 ; i<nrsubdomains; i++)
   {
      if (myid != subdomain_rank[i]) continue;
      x[i].SetSize(fes[i]->GetTrueVSize());
      GridFunction gf(fes[i]);
      gf = 0.0;

      if (i==3) gf.ProjectCoefficient(c1);
      x[i] = gf;
   }

   SubdomainsToGlobal(x,y);

   // cout << "1: myid = " <<  myid << ", y = "; y.Print();

   string keys = "keys amrRljc\n";
   ParGridFunction pgf(pfes);

   const Operator &P = *pfes->GetProlongationMatrix();
   P.Mult(y, pgf);
   

   // cout << "pgf size = " << pgf.Size() << endl;
   // cout << "y   size = " << y.Size() << endl;

   // cout << "2: myid = " <<  myid << ", pgf = "; pgf.Print();
   char vishost[] = "localhost";
   int  visport   = 19916;
   socketstream sol_sock(vishost, visport);
   sol_sock.precision(8);
   sol_sock << "parallel " << num_procs << " " << myid << "\n"
               << "solution\n" << *pfes->GetParMesh() << pgf 
               << keys << flush;


   ParGridFunction pgf1(pfes);
   pgf1.ProjectCoefficient(c1);
   Vector y1(pfes->GetTrueVSize());
   const SparseMatrix * R = pfes->GetRestrictionMatrix();

   R->Mult(pgf1,y1);
   // P.MultTranspose(pgf1,y1);
   std::vector<Vector> x1(nrsubdomains);
   GlobalToSubdomains(y1,x1);


   // for (int i = 0 ; i<nrsubdomains; i++)
   // {
   //    if (myid != subdomain_rank[i]) continue;
   //    ostringstream mesh_name;
   //    mesh_name << "output/mesh." << setfill('0') << setw(6) << i;
   //    ofstream mesh_ofs(mesh_name.str().c_str());
   //    mesh_ofs.precision(8);
   //    fes[i]->GetMesh()->Print(mesh_ofs);
   //    GridFunction gf(fes[i]);
   //    gf = x1[i];
   //    ostringstream gf_name;
   //    gf_name << "output/gf." << setfill('0') << setw(6) << i;
   //    ofstream gf_ofs(gf_name.str().c_str());
   //    gf_ofs.precision(8);
   //    gf.Save(gf_ofs);
   // }


   for (int i = 0 ; i<nrsubdomains; i++)
   {
      if (myid == subdomain_rank[i])
      {
         socketstream sol_sock1(vishost, visport);
         sol_sock1.precision(8);
         sol_sock1 << "parallel " << nrsubdomains << " " << i << "\n";
         GridFunction * gf = new GridFunction(fes[i]);
         *gf = x1[i];
         sol_sock1 << "solution\n" << *fes[i]->GetMesh() << *gf << flush;
      }
      MPI_Barrier(MPI_COMM_WORLD);
   }


   socketstream gf_sock(vishost, visport);
   gf_sock.precision(8);
   gf_sock << "parallel " << num_procs << " " << myid << "\n"
           << "solution\n" << *pfes->GetParMesh() << pgf1 
           << keys << flush;
}

