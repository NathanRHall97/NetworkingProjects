#include "messages.h"

RoutingMessage::RoutingMessage()
{}

RoutingMessage::RoutingMessage(const RoutingMessage &rhs) {
    *this = rhs;
}

#if defined(LINKSTATE)
RoutingMessage::RoutingMessage(map<unsigned, map<unsigned, TopoLink> > topo)
{
    this->topo = topo;
}
#endif

#if defined(DISTANCEVECTOR)
RoutingMessage::RoutingMessage(unsigned from_node, unsigned seq_num, map<unsigned, double> distance_vector)
{
    this->from_node = from_node;
    this->seq_num = seq_num;
    this->distance_vector = distance_vector;
}
#endif

#if defined(LINKSTATE)
RoutingMessage & RoutingMessage::operator=(const RoutingMessage & rhs) {
    return *(new RoutingMessage(rhs.topo));
}
#endif

#if defined(DISTANCEVECTOR)
RoutingMessage & RoutingMessage::operator=(const RoutingMessage & rhs) {
    return *(new RoutingMessage(rhs.from_node, rhs.seq_num, rhs.distance_vector));
}
#endif

#if defined(GENERIC)
ostream &RoutingMessage::Print(ostream &os) const
{
    os << "Generic RoutingMessage()";
    return os;
}
#endif

#if defined(LINKSTATE)
ostream &RoutingMessage::Print(ostream &os) const
{
    os << "LinkState RoutingMessage()" << endl;
    os << "Topo:" << endl;
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

#if defined(DISTANCEVECTOR)
ostream &RoutingMessage::Print(ostream &os) const
{
    os << "DistanceVector RoutingMessage()\n";
    os << "Sequence number: " << seq_num << endl;
    os << "Distance vector from Node " << from_node << ":\n";
    os << "Destination\t\tCost\n";
    for (map<unsigned, double>::const_iterator itr = distance_vector.begin(); itr != distance_vector.end(); ++itr)
    {
        os << itr->first << "\t\t" << itr->second << "\n";
    }
    return os;
}
#endif
