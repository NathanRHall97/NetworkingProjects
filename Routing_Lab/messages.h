#ifndef _messages
#define _messages

#include <iostream>
#include "node.h"
#include "link.h"

struct RoutingMessage {
    RoutingMessage();
    RoutingMessage(const RoutingMessage &rhs);
    #if defined(DISTANCEVECTOR)
    RoutingMessage(unsigned from_node, unsigned seq_num, map<unsigned, double> distance_vector);
    #endif
    #if defined(LINKSTATE)
    RoutingMessage(map<unsigned, map<unsigned, TopoLink> > topo);
    #endif

    RoutingMessage &operator=(const RoutingMessage &rhs);

    ostream & Print(ostream &os) const;

    // Anything else you need

    #if defined(LINKSTATE)
    map<unsigned, map<unsigned, TopoLink> > topo;   // node local representation of topology
    #endif
    #if defined(DISTANCEVECTOR)
    unsigned from_node; // identifier of node sending the message
    unsigned seq_num;   // to recognize out of date RoutingMessages (there are no guarantees of in-order delivery)
    map<unsigned, double> distance_vector;
    #endif
};

inline ostream & operator<<(ostream &os, const RoutingMessage & m) { return m.Print(os);}

#endif
