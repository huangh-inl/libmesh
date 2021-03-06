// The libMesh Finite Element Library.
// Copyright (C) 2002-2014 Benjamin S. Kirk, John W. Peterson, Roy H. Stogner

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#include "libmesh/checkpoint_io.h"

// C++ includes
#include <iostream>
#include <iomanip>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <sstream> // for ostringstream

// Local includes
#include "libmesh/xdr_io.h"
#include "libmesh/legacy_xdr_io.h"
#include "libmesh/xdr_cxx.h"
#include "libmesh/enum_xdr_mode.h"
#include "libmesh/mesh_base.h"
#include "libmesh/node.h"
#include "libmesh/elem.h"
#include "libmesh/boundary_info.h"
#include "libmesh/parallel.h"
#include "libmesh/mesh_tools.h"
#include "libmesh/partitioner.h"
#include "libmesh/libmesh_logging.h"
#include "libmesh/mesh_communication.h"
#include "libmesh/parallel_mesh.h"

namespace libMesh
{

// ------------------------------------------------------------
// CheckpointIO members
CheckpointIO::CheckpointIO (MeshBase& mesh, const bool binary_in) :
  MeshInput<MeshBase> (mesh,/* is_parallel_format = */ true),
  MeshOutput<MeshBase>(mesh,/* is_parallel_format = */ true),
  ParallelObject      (mesh),
  _binary             (binary_in),
  _version            ("checkpoint-1.0")
{
}



CheckpointIO::CheckpointIO (const MeshBase& mesh, const bool binary_in) :
  MeshOutput<MeshBase>(mesh,/* is_parallel_format = */ true),
  ParallelObject      (mesh),
  _binary (binary_in)
{
}



CheckpointIO::~CheckpointIO ()
{
}




void CheckpointIO::write (const std::string& name)
{
  START_LOG("write()","CheckpointIO");

  // convenient reference to our mesh
  const MeshBase &mesh = MeshOutput<MeshBase>::mesh();

  // Try to dynamic cast the mesh to see if it's a ParallelMesh object
  // Note: Just using is_serial() is not good enough because the Mesh won't
  // have been prepared yet when is when that flag gets set to false... sigh.
  bool parallel_mesh = dynamic_cast<const ParallelMesh*>(&mesh);

  // If this is a serial mesh then we're only going to write it on processor 0
  if(parallel_mesh || this->processor_id() == 0)
    {
      std::ostringstream file_name_stream;

      file_name_stream << name;

      if(parallel_mesh)
        file_name_stream << "-" << this->processor_id();

      Xdr io (file_name_stream.str(), this->binary() ? ENCODE : WRITE);

      // write the version
      io.data(_version, "# version");

      // Write out whether or not this is a serial mesh (helps with error checking on read)
      {
        unsigned int parallel = parallel_mesh;
        io.data(parallel, "# parallel");
      }

      // If we're writing out a parallel mesh then we need to write the number of processors
      // so we can check it upon reading the file
      if(parallel_mesh)
        {
          largest_id_type n_procs = this->n_processors();
          io.data(n_procs, "# n_procs");
        }

      // write subdomain names
      this->write_subdomain_names(io);

      // write the nodal locations
      this->write_nodes (io);

      // write connectivity
      this->write_connectivity (io);

      // write the boundary condition information
      this->write_bcs (io);

      // write the nodeset information
      this->write_nodesets (io);

      // pause all processes until the writing ends -- this will
      // protect for the pathological case where a write is
      // followed immediately by a read.  The write must be
      // guaranteed to complete first.
      io.close();
    }

  this->comm().barrier();

  STOP_LOG("write()","CheckpointIO");
}



void CheckpointIO::write_subdomain_names(Xdr &io) const
{
  {
    const MeshBase &mesh = MeshOutput<MeshBase>::mesh();

    const std::map<subdomain_id_type, std::string> & subdomain_map = mesh.get_subdomain_name_map();

    std::vector<header_id_type> subdomain_ids;   subdomain_ids.reserve(subdomain_map.size());
    std::vector<std::string>  subdomain_names; subdomain_names.reserve(subdomain_map.size());

    // We need to loop over the map and make sure that there aren't any invalid entries.  Since we
    // return writable references in mesh_base, it's possible for the user to leave some entity names
    // blank.  We can't write those to the XDA file.
    header_id_type n_subdomain_names = 0;
    std::map<subdomain_id_type, std::string>::const_iterator it_end = subdomain_map.end();
    for (std::map<subdomain_id_type, std::string>::const_iterator it = subdomain_map.begin(); it != it_end; ++it)
      {
        if (!it->second.empty())
          {
            n_subdomain_names++;
            subdomain_ids.push_back(it->first);
            subdomain_names.push_back(it->second);
          }
      }

    io.data(n_subdomain_names, "# subdomain id to name map");
    // Write out the ids and names in two vectors
    if (n_subdomain_names)
      {
        io.data(subdomain_ids);
        io.data(subdomain_names);
      }
  }
}



void CheckpointIO::write_nodes (Xdr &io) const
{
  // convenient reference to our mesh
  const MeshBase &mesh = MeshOutput<MeshBase>::mesh();

  MeshBase::const_node_iterator
    it  = mesh.nodes_begin(),
    end = mesh.nodes_end();

  unsigned int n_nodes_here = MeshTools::n_nodes(it, end);

  io.data(n_nodes_here, "# n_nodes on proc");

  it = mesh.nodes_begin();

  // Will hold the node id and pid
  std::vector<largest_id_type> id_pid(2);

  // For the coordinates
  std::vector<Real> coords(LIBMESH_DIM);

  for(; it != end; ++it)
    {
      Node & node = *(*it);

      id_pid[0] = node.id();
      id_pid[1] = node.processor_id();

      io.data_stream(&id_pid[0], 2, 2);

#ifdef LIBMESH_ENABLE_UNIQUE_ID
      largest_id_type unique_id = node.unique_id();

      io.data(unique_id, "# unique id");
#endif

      coords[0] = node(0);

#if LIBMESH_DIM > 1
      coords[1] = node(1);
#endif

#if LIBMESH_DIM > 2
      coords[2] = node(2);
#endif

      io.data_stream(&coords[0], LIBMESH_DIM, 3);
    }
}



void CheckpointIO::write_connectivity (Xdr &io) const
{
  libmesh_assert (io.writing());

  // convenient reference to our mesh
  const MeshBase &mesh = MeshOutput<MeshBase>::mesh();

  // We will only write active elements and their parents.
  unsigned int n_active_levels = n_active_levels_on_processor(mesh);

  std::vector<xdr_id_type> n_elem_at_level(n_active_levels);

  // Find the number of elements at each level
  for (unsigned int level=0; level<n_active_levels; level++)
    {
      MeshBase::const_element_iterator it  = mesh.level_elements_begin(level);
      MeshBase::const_element_iterator end = mesh.level_elements_end(level);

      n_elem_at_level[level] = MeshTools::n_elem(it, end);
    }

  io.data(n_active_levels, "# n_active_levels");

  for(unsigned int level=0; level < n_active_levels; level++)
    {
      std::ostringstream comment;
      comment << "# n_elem at level ";
      comment << level ;
      io.data (n_elem_at_level[level], comment.str().c_str());

      MeshBase::const_element_iterator it  = mesh.level_elements_begin(level);
      MeshBase::const_element_iterator end = mesh.level_elements_end(level);
      for (; it != end; ++it)
        {
          Elem & elem = *(*it);

          unsigned int n_nodes = elem.n_nodes();

          // id type pid subdomain_id parent_id
          std::vector<largest_id_type> elem_data(5);

          elem_data[0] = elem.id();
          elem_data[1] = elem.type();
          elem_data[2] = elem.processor_id();
          elem_data[3] = elem.subdomain_id();

          if(elem.parent() != NULL)
            elem_data[4] = elem.parent()->id();
          else
            elem_data[4] = DofObject::invalid_processor_id;

          std::vector<largest_id_type> conn_data(n_nodes);

          for(unsigned int i=0; i<n_nodes; i++)
            conn_data[i] = elem.node(i);

          io.data_stream(&elem_data[0], elem_data.size(), elem_data.size());

#ifdef LIBMESH_ENABLE_UNIQUE_ID
          largest_id_type unique_id = elem.unique_id();

          io.data(unique_id, "# unique id");
#endif

#ifdef LIBMESH_ENABLE_AMR
          unsigned int p_level = elem.p_level();

          io.data(p_level, "# p_level");
#endif

          io.data_stream(&conn_data[0], conn_data.size(), conn_data.size());
        }
    }
}



void CheckpointIO::write_bcs (Xdr &io) const
{
  libmesh_assert (io.writing());

  // convenient reference to our mesh
  const MeshBase &mesh = MeshOutput<MeshBase>::mesh();

  // and our boundary info object
  const BoundaryInfo &boundary_info = *mesh.boundary_info;

  // Version 0.9.2+ introduces entity names
  write_bc_names(io, boundary_info, true);  // sideset names
  write_bc_names(io, boundary_info, false); // nodeset names

  std::vector<dof_id_type> element_id_list;
  std::vector<unsigned short int> side_list;
  std::vector<boundary_id_type> bc_id_list;

  boundary_info.build_side_list(element_id_list, side_list, bc_id_list);

  io.data(element_id_list, "# element ids for bcs");
  io.data(side_list, "# sides of elements for bcs");
  io.data(bc_id_list, "# bc ids");
}



void CheckpointIO::write_nodesets (Xdr &io) const
{
  libmesh_assert (io.writing());

  // convenient reference to our mesh
  const MeshBase &mesh = MeshOutput<MeshBase>::mesh();

  // and our boundary info object
  const BoundaryInfo &boundary_info = *mesh.boundary_info;

  std::vector<dof_id_type> node_id_list;
  std::vector<boundary_id_type> bc_id_list;

  boundary_info.build_node_list(node_id_list, bc_id_list);

  io.data(node_id_list, "# node id list");
  io.data(bc_id_list, "# nodeset bc id list");
}



void CheckpointIO::write_bc_names (Xdr &io, const BoundaryInfo & info, bool is_sideset) const
{
  const std::map<boundary_id_type, std::string> & boundary_map = is_sideset ?
    info.get_sideset_name_map() : info.get_nodeset_name_map();

  std::vector<boundary_id_type> boundary_ids;   boundary_ids.reserve(boundary_map.size());
  std::vector<std::string>  boundary_names; boundary_names.reserve(boundary_map.size());

  // We need to loop over the map and make sure that there aren't any invalid entries.  Since we
  // return writable references in boundary_info, it's possible for the user to leave some entity names
  // blank.  We can't write those to the XDA file.
  header_id_type n_boundary_names = 0;
  std::map<boundary_id_type, std::string>::const_iterator it_end = boundary_map.end();
  for (std::map<boundary_id_type, std::string>::const_iterator it = boundary_map.begin(); it != it_end; ++it)
    {
      if (!it->second.empty())
        {
          n_boundary_names++;
          boundary_ids.push_back(it->first);
          boundary_names.push_back(it->second);
        }
    }

  if (is_sideset)
    io.data(n_boundary_names, "# sideset id to name map");
  else
    io.data(n_boundary_names, "# nodeset id to name map");
  // Write out the ids and names in two vectors
  if (n_boundary_names)
    {
      io.data(boundary_ids);
      io.data(boundary_names);
    }
}



void CheckpointIO::read (const std::string& name)
{
  START_LOG("read()","CheckpointIO");

  MeshBase &mesh = MeshInput<MeshBase>::mesh();

  // Try to dynamic cast the mesh to see if it's a ParallelMesh object
  // Note: Just using is_serial() is not good enough because the Mesh won't
  // have been prepared yet when is when that flag gets set to false... sigh.
  bool parallel_mesh = dynamic_cast<ParallelMesh*>(&mesh);

  // If this is a serial mesh then we're going to only read it on processor 0 and broadcast it
  if(parallel_mesh || this->processor_id() == 0)
    {
      std::ostringstream file_name_stream;

      file_name_stream << name;

      if(parallel_mesh)
        file_name_stream << "-" << this->processor_id();

      {
        std::ifstream in (file_name_stream.str().c_str());

        if (!in.good())
          {
            libMesh::err << "ERROR: cannot locate specified file:\n\t"
                         << file_name_stream.str()
                         << std::endl;
            libmesh_error();
          }
      }

      Xdr io (file_name_stream.str(), this->binary() ? DECODE : READ);

      // read the version
      io.data (_version);

      // Check if the mesh we're reading is the same as the one that was written
      {
        unsigned int parallel;
        io.data(parallel, "# parallel");

        if(static_cast<unsigned int>(parallel_mesh) != parallel)
          {
            libMesh::err << "Attempted to utilize a checkpoint file with an incompatible mesh distribution!" << std::endl;
            libmesh_error();
          }
      }

      // If this is a parallel mesh then we need to check to ensure we're reading this on the same number of procs
      if(parallel_mesh)
        {
          largest_id_type n_procs;
          io.data(n_procs, "# n_procs");

          if(n_procs != this->n_processors())
            {
              libMesh::err << "Attempted to utilize a checkpoint file on " << this->n_processors() << " processors but it was written using " << n_procs << "!!";
              libmesh_error();
            }
        }

      // read subdomain names
      this->read_subdomain_names(io);

      // read the nodal locations
      this->read_nodes (io);

      // read connectivity
      this->read_connectivity (io);

      // read the boundary conditions
      this->read_bcs (io);

      // read the nodesets
      this->read_nodesets (io);

      io.close();
    }

  // If the mesh is serial then we only read it on processor 0 so we need to broadcast it
  if(!parallel_mesh)
    MeshCommunication().broadcast(mesh);

  STOP_LOG("read()","CheckpointIO");
}



void CheckpointIO::read_subdomain_names(Xdr &io)
{
  MeshBase &mesh = MeshInput<MeshBase>::mesh();

  std::map<subdomain_id_type, std::string> & subdomain_map = mesh.set_subdomain_name_map();

  std::vector<header_id_type> subdomain_ids;   subdomain_ids.reserve(subdomain_map.size());
  std::vector<std::string>  subdomain_names; subdomain_names.reserve(subdomain_map.size());

  header_id_type n_subdomain_names = 0;
  io.data(n_subdomain_names, "# subdomain id to name map");

  if(n_subdomain_names)
    {
      io.data(subdomain_ids);
      io.data(subdomain_names);

      for(unsigned int i=0; i<subdomain_ids.size(); i++)
        subdomain_map[subdomain_ids[i]] = subdomain_names[i];
    }
}



void CheckpointIO::read_nodes (Xdr &io)
{
  // convenient reference to our mesh
  MeshBase &mesh = MeshInput<MeshBase>::mesh();

  unsigned int n_nodes_here;
  io.data(n_nodes_here, "# n_nodes on proc");

  // Will hold the node id and pid
  std::vector<largest_id_type> id_pid(2);

  // For the coordinates
  std::vector<Real> coords(LIBMESH_DIM);

  for(unsigned int i=0; i<n_nodes_here; i++)
    {
      io.data_stream(&id_pid[0], 2, 2);

#ifdef LIBMESH_ENABLE_UNIQUE_ID
      largest_id_type unique_id = 0;
      io.data(unique_id, "# unique id");
#endif

      io.data_stream(&coords[0], LIBMESH_DIM, LIBMESH_DIM);

      Point p;
      p(0) = coords[0];

#if LIBMESH_DIM > 1
      p(1) = coords[1];
#endif

#if LIBMESH_DIM > 2
      p(2) = coords[2];
#endif

#ifdef LIBMESH_ENABLE_UNIQUE_ID
      Node * node =
#endif
        mesh.add_point(p, id_pid[0], id_pid[1]);

#ifdef LIBMESH_ENABLE_UNIQUE_ID
      node->set_unique_id() = unique_id;
#endif
    }
}



void CheckpointIO::read_connectivity (Xdr &io)
{
  // convenient reference to our mesh
  MeshBase &mesh = MeshInput<MeshBase>::mesh();

  unsigned int n_active_levels;
  io.data(n_active_levels, "# n_active_levels");

  // Keep track of the highest dimensional element we've added to the mesh
  unsigned int highest_elem_dim = 1;

  for(unsigned int level=0; level < n_active_levels; level++)
    {
      xdr_id_type n_elem_at_level = 0;
      io.data (n_elem_at_level, "");

      for (unsigned int i=0; i<n_elem_at_level; i++)
        {
          // id type pid subdomain_id parent_id
          std::vector<largest_id_type> elem_data(5);
          io.data_stream(&elem_data[0], elem_data.size(), elem_data.size());

#ifdef LIBMESH_ENABLE_UNIQUE_ID
          largest_id_type unique_id = 0;
          io.data(unique_id, "# unique id");
#endif

#ifdef LIBMESH_ENABLE_AMR
          unsigned int p_level = 0;

          io.data(p_level, "# p_level");
#endif

          unsigned int n_nodes = Elem::type_to_n_nodes_map[elem_data[1]];

          // Snag the node ids this element was connected to
          std::vector<largest_id_type> conn_data(n_nodes);
          io.data_stream(&conn_data[0], conn_data.size(), conn_data.size());

          const dof_id_type id                 = elem_data[0];
          const ElemType elem_type             = static_cast<ElemType>(elem_data[1]);
          const processor_id_type processor_id = elem_data[2];
          const subdomain_id_type subdomain_id = elem_data[3];
          const dof_id_type parent_id          = elem_data[4];

          Elem * parent = (parent_id == DofObject::invalid_processor_id) ? NULL : mesh.elem(parent_id);

          // Create the element
          Elem * elem = Elem::build(elem_type, parent).release();

#ifdef LIBMESH_ENABLE_UNIQUE_ID
          elem->set_unique_id() = unique_id;
#endif

          if(elem->dim() > highest_elem_dim)
            highest_elem_dim = elem->dim();

          elem->set_id()       = id;
          elem->processor_id() = processor_id;
          elem->subdomain_id() = subdomain_id;

#ifdef LIBMESH_ENABLE_AMR
          elem->hack_p_level(p_level);

          // Set parent connections
          if(parent)
            {
              parent->add_child(elem);
              parent->set_refinement_flag (Elem::INACTIVE);
              elem->set_refinement_flag   (Elem::JUST_REFINED);
            }
#endif

          libmesh_assert(elem->n_nodes() == conn_data.size());

          // Connect all the nodes to this element
          for (unsigned int n=0; n<conn_data.size(); n++)
            elem->set_node(n) = mesh.node_ptr(conn_data[n]);

          mesh.add_elem(elem);
        }
    }

  mesh.set_mesh_dimension(highest_elem_dim);
}



void CheckpointIO::read_bcs (Xdr &io)
{
  // convenient reference to our mesh
  MeshBase &mesh = MeshInput<MeshBase>::mesh();

  // and our boundary info object
  BoundaryInfo &boundary_info = *mesh.boundary_info;

  // Version 0.9.2+ introduces entity names
  read_bc_names(io, boundary_info, true);  // sideset names
  read_bc_names(io, boundary_info, false); // nodeset names

  std::vector<dof_id_type> element_id_list;
  std::vector<unsigned short int> side_list;
  std::vector<boundary_id_type> bc_id_list;

  io.data(element_id_list, "# element ids for bcs");
  io.data(side_list, "# sides of elements for bcs");
  io.data(bc_id_list, "# bc ids");

  for(unsigned int i=0; i<element_id_list.size(); i++)
    boundary_info.add_side(element_id_list[i], side_list[i], bc_id_list[i]);
}



void CheckpointIO::read_nodesets (Xdr &io)
{
  // convenient reference to our mesh
  MeshBase &mesh = MeshInput<MeshBase>::mesh();

  // and our boundary info object
  BoundaryInfo &boundary_info = *mesh.boundary_info;

  std::vector<dof_id_type> node_id_list;
  std::vector<boundary_id_type> bc_id_list;

  io.data(node_id_list, "# node id list");
  io.data(bc_id_list, "# nodeset bc id list");

  for(unsigned int i=0; i<node_id_list.size(); i++)
    boundary_info.add_node(node_id_list[i], bc_id_list[i]);
}



void CheckpointIO::read_bc_names(Xdr &io, BoundaryInfo & info, bool is_sideset)
{
  std::map<boundary_id_type, std::string> & boundary_map = is_sideset ?
    info.set_sideset_name_map() : info.set_nodeset_name_map();

  std::vector<boundary_id_type> boundary_ids;
  std::vector<std::string>  boundary_names;

  header_id_type n_boundary_names = 0;

  if (is_sideset)
    io.data(n_boundary_names, "# sideset id to name map");
  else
    io.data(n_boundary_names, "# nodeset id to name map");

  if (n_boundary_names)
    {
      io.data(boundary_ids);
      io.data(boundary_names);
    }

  // Add them back into the map
  for(unsigned int i=0; i<boundary_ids.size(); i++)
    boundary_map[boundary_ids[i]] = boundary_names[i];
}


unsigned int CheckpointIO::n_active_levels_on_processor(const MeshBase &mesh) const
{
  unsigned int max_level = 0;

  MeshBase::const_element_iterator el = mesh.active_elements_begin();
  const MeshBase::const_element_iterator end_el = mesh.active_elements_end();

  for( ; el != end_el; ++el)
    max_level = std::max((*el)->level(), max_level);

  return max_level + 1;
}

} // namespace libMesh
