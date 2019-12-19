#include "table.h"

#include <stdexcept>
#include <cstdio> // for sprintf()

#if defined(DISTANCEVECTOR)
Table::Table() {
    distance_vectors.clear();
}
#endif

#if defined(LINKSTATE)
Table::Table() {
  topo.clear();
}
#endif

Table::Table(const Table & rhs) {
    *this = rhs;
}

#if defined(DISTANCEVECTOR)
// copies the rhs object into the lhs
Table & Table::operator=(const Table & rhs) {
    distance_vectors = rhs.distance_vectors;
    return *this;
}
#endif

#if defined(LINKSTATE)
Table & Table::operator=(const Table & rhs) {
  topo = rhs.topo;
  return *this;
}
#endif

#if defined(DISTANCEVECTOR)
// Returns the distance vector containing costs of paths beginning at src_node. Throws an exception if this distance vector does not exist in the table
map<unsigned, double> Table::GetDistanceVector(unsigned src_node) const
{
  map<unsigned, map<unsigned, double> >::const_iterator itr = distance_vectors.find(src_node);
  if (itr != distance_vectors.end()) {  // found the distance vector for src_node
    return itr->second;
  } else {  // did not find the distance vector for src_node
    throw invalid_argument("Tried to access a distance vector that does not exist in the table");
  }
}

// Inserts distance_vector into the table as src_node's distance vector
void Table::SetDistanceVector(unsigned src_node, map<unsigned, double> distance_vector)
{
  distance_vectors[src_node] = distance_vector;
}

// Returns true if and only if the routing table currently contains a distance vector for src_node
bool Table::HasDistanceVector(unsigned src_node) const
{
   map<unsigned, map<unsigned, double> >::const_iterator found_distance_vector = distance_vectors.find(src_node);
   return found_distance_vector != distance_vectors.end();
}
#endif

#if defined(LINKSTATE)
// Returns the node local topology that currently exists in the table
map<unsigned, map<unsigned, TopoLink> > Table::GetTopo() const
{
  return topo;
}

// Returns the link from src to dest in the node local topology if such a link exists. Throws an exception if no such link exists
TopoLink Table::GetTopoLink(unsigned src, unsigned dest) const
{
  map<unsigned, map<unsigned, TopoLink> >::const_iterator src_itr = topo.find(src);
  if (src_itr == topo.end()) {  // there are no links beginning at src in topo
    throw "No link found beginning at src in GetTopoLink()";
  }

  map<unsigned, TopoLink>::const_iterator dest_itr = src_itr->second.find(dest);
  if (dest_itr == src_itr->second.end()) { // there are no links from src to dest in topo
    throw "No link found beginning at src and ending at dest in GetTopoLink()";
  }

  return dest_itr->second;
}

// Sets the link from src to dest to link in the node local topology
void Table::SetTopoLink(unsigned src, unsigned dest, TopoLink link)
{
  topo[src][dest] = link;
}

// Returns true if and only if the node local topology contains a link between src and dest
bool Table::HasTopoLink(unsigned src, unsigned dest) const
{
  map<unsigned, map<unsigned, TopoLink> >::const_iterator src_itr = topo.find(src);
  if (src_itr == topo.end()) {  // there are no links beginning at src in topo
    return false;
  }

  map<unsigned, TopoLink>::const_iterator dest_itr = src_itr->second.find(dest);
  if (dest_itr == src_itr->second.end()) { // there are no links from src to dest in topo
    return false;
  }

  return true;
}

// Returns the collection of links in the node local topology which begin at src. Throws an exception if src has no outgoing links
map<unsigned, TopoLink> Table::GetOutgoingTopoLinks(unsigned src) const
{
  map<unsigned, map<unsigned, TopoLink> >::const_iterator src_itr = topo.find(src);
  if (src_itr == topo.end()) {  // there are no links beginning at src in topo
    throw "No link found beginning at src in GetOutgoingTopoLinks()";
  }

  return src_itr->second;
}

// Returns true if and only if src has at least one outgoing link in the node local topology
bool Table::HasOutgoingTopoLinks(unsigned src) const
{
  map<unsigned, map<unsigned, TopoLink> >::const_iterator src_itr = topo.find(src);
  if (src_itr == topo.end()) {  // there are no links beginning at src in topo
    return false;
  }

  return true;
}
#endif

#if defined(DISTANCEVECTOR)
ostream & Table::Print(ostream &os) const
{
  os << "Distance Vector Table()\n";

  for (map<unsigned, map<unsigned, double> >::const_iterator src_itr = distance_vectors.begin(); src_itr != distance_vectors.end(); ++src_itr)
  {
    os << "Source: " << src_itr->first << "\n";
    os << "Destination\t\tLeast Cost\n";
    for (map<unsigned, double>::const_iterator dest_itr = src_itr->second.begin(); dest_itr != src_itr->second.end(); ++dest_itr)
    {
      os << dest_itr->first << "\t\t" << dest_itr->second << "\n";
    }
  }

  return os;
}
#endif

#if defined(LINKSTATE)
ostream & Table::Print(ostream &os) const
{
  os << "Link State Table()" << endl;

  for (map<unsigned, map<unsigned, TopoLink> >::const_iterator src_itr = topo.begin(); src_itr != topo.end(); ++src_itr)
  {
      os << "Source node: " << src_itr->first << endl;
      os << "Destination\t\tCost\t\tAge" << endl;
      for (map<unsigned, TopoLink>::const_iterator dest_itr = src_itr->second.begin(); dest_itr != src_itr->second.end(); ++dest_itr)
      {
          os << dest_itr->first << "\t\t" << dest_itr->second.cost << "\t\t" << dest_itr->second.age << endl;
      }
  }

  return os;
}
#endif
