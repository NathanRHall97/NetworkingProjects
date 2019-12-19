#include "linkstate.h"
#include <cfloat>    // for max value of double
#include <set>  // for set of unvisited nodes in Dijkstra's algorithm

LinkState::LinkState(unsigned n, SimulationContext* c, double b, double l) :
    Node(n, c, b, l)
{}

LinkState::LinkState(const LinkState & rhs) :
    Node(rhs)
{
    *this = rhs;
}

LinkState & LinkState::operator=(const LinkState & rhs) {
    Node::operator=(rhs);
    return *this;
}

LinkState::~LinkState() {}

// Updates the node local topology then broadcasts it. Assumes the link update did change the latency of the link.
void LinkState::LinkHasBeenUpdated(Link* l) {
    cerr << *this << ": Link Update: " << *l << endl;

    // update routing table
    if (routing_table.HasTopoLink(l->GetSrc(), l->GetDest())) { // this link already existed prior to the update
        TopoLink topo_link = routing_table.GetTopoLink(l->GetSrc(), l->GetDest());
        topo_link.cost = l->GetLatency();
        topo_link.age++;    // increment previous age
        routing_table.SetTopoLink(l->GetSrc(), l->GetDest(), topo_link);
    } else {    // this link did not exist prior to the update
        TopoLink topo_link;
        topo_link.cost = l->GetLatency();
        topo_link.age = 0;  // initialize age to zero
        routing_table.SetTopoLink(l->GetSrc(), l->GetDest(), topo_link);
    }

    SendToNeighbors(new RoutingMessage(routing_table.GetTopo()));
}

// Updates node local topology with any newer links. Broadcasts topology if and only if node local topology 
void LinkState::ProcessIncomingRoutingMessage(RoutingMessage *m) {
    cerr << *this << " got a routing message: " << *m << endl;

    bool topo_changed = false;

    for (map<unsigned, map<unsigned, TopoLink> >::const_iterator src_itr = m->topo.begin(); src_itr != m->topo.end(); ++src_itr)
    {
        for (map<unsigned, TopoLink>::const_iterator dest_itr = src_itr->second.begin(); dest_itr != src_itr->second.end(); ++dest_itr)
        {
            unsigned src = src_itr->first;
            unsigned dest = dest_itr->first;
            TopoLink received_link = dest_itr->second;

            if (routing_table.HasTopoLink(src, dest)) {   // this node's topology contains the same link
                // compare ages, updating if the received link is newer (higher age)
                TopoLink existing_link = routing_table.GetTopoLink(src, dest);
                if (existing_link.age < received_link.age) {  // the received link is newer
                    routing_table.SetTopoLink(src, dest, received_link);
                    topo_changed = true;
                }
            } else {    // this node's topology does not yet contain this link
                // add this link to the local topology
                routing_table.SetTopoLink(src, dest, received_link);
                topo_changed = true;
            }
        }
    }

    // broadcast topology if and only if it changed as a result of receiving the message
    if (topo_changed) {
        SendToNeighbors(new RoutingMessage(routing_table.GetTopo()));
    }
}

// Note that while OSPF uses timeouts to rebroadcast link state at least once every 30 minutes for improved robustness (see p. 393 of the textbook), this behavior would cause the routelab simulation to never end since an event would always be pending
// Thus we have decided not to use timeouts as a part of our link state algorithm
void LinkState::TimeOut() {
    cerr << *this << " got a timeout: (ignored)" << endl;
}

// Returns the next hop from this node to destination. Uses Dijkstra's algorithm on the node local topology to find the shortest path between this node and source destination, then the next hop is read off of the shortest path
// Note that the node local topology is a collection of links, so the set of nodes in the network is not known beforehand
Node* LinkState::GetNextHop(Node *destination) { 
    map<unsigned, double> distance; // distance[node] is the minimum known distance from source to node
    distance[number] = 0;   // distance from source to source is zero
    map<unsigned, unsigned> previous;   // previous[next_node] is the node before next_node on the shortest path from source to destination

    set<unsigned> visited;  // set of visited nodes (necessary because all nodes in topology are not known beforehand, so a node not in unvisited could either be not yet seen or already visited)
    set<unsigned> unvisited;    // set of unvisited but seen nodes 
    unvisited.insert(number);   // this node is unvisited before algorithm starts

    while (unvisited.size() > 0)
    {
        // find node with minimum distance
        unsigned min_node;
        double min_distance = DBL_MAX;
        for (map<unsigned, double>::const_iterator dist_itr = distance.begin(); dist_itr != distance.end(); ++dist_itr)
        {
            unsigned temp_node = dist_itr->first;
            double temp_distance = dist_itr->second;

            // only check distance to unvisited nodes
            set<unsigned>::const_iterator unvisited_itr = unvisited.find(temp_node);
            if (unvisited_itr == unvisited.end()) { // this node is not unvisited
                continue;   // only compare distances for unvisited nodes
            }

            if (temp_distance < min_distance) { // found an unvisited node with lesser distance
                min_node = temp_node;
                min_distance = dist_itr->second;
            }
        }

        if (min_distance == DBL_MAX) {
            throw "No node with minimum distance found";
        }

        if (min_node == destination->GetNumber()) { // found destination
            break;  // have shortest path, now determine next hop
        }

        // visit this node with minimum distance
        visited.insert(min_node);
        unvisited.erase(min_node);

        // iterate through neighbors of min_node
        if (routing_table.HasOutgoingTopoLinks(min_node)) { // min_node has neighbors
            map<unsigned, TopoLink> outgoing_links = routing_table.GetOutgoingTopoLinks(min_node);
            for (map<unsigned, TopoLink>::const_iterator link_itr = outgoing_links.begin(); link_itr != outgoing_links.end(); ++link_itr)
            {
                unsigned neighbor = link_itr->first;
                double cost_to_neighbor = link_itr->second.cost;

                set<unsigned>::const_iterator visited_itr = visited.find(neighbor);
                if (visited_itr == visited.end()) { // this neighbor has not yet been visited
                    unvisited.insert(neighbor);  // mark this neighbor as unvisited; note that if this neighbor has already been marked as unvisited this line has no effect
                }

                double temp_distance = min_distance + cost_to_neighbor;

                map<unsigned, double>::const_iterator dist_itr = distance.find(neighbor);   // see if we do not yet know the distance to this neighbor
                bool lesser_distance = dist_itr == distance.end() || temp_distance < distance[link_itr->first]; // distance not yet known or distance through this node is lower than least known distance

                if (lesser_distance) {  // we found a lesser cost path to neighbor through min_node
                    distance[neighbor] = temp_distance;
                    previous[neighbor] = min_node;
                }
            }
        }
    }

    // use previous to find next hop from source to destination
    unsigned curr = destination->GetNumber();
    unsigned prev = curr;
    while (prev != number)
    {
        curr = prev;
        map<unsigned, unsigned>::const_iterator prev_itr = previous.find(curr);
        if (prev_itr == previous.end()) {   // curr is not on the least cost path from source to destination
            throw "No path to destination found";
        }

        prev = prev_itr->second;
    }

    unsigned next_hop_num = curr;   // prev == source, so curr is number of next hop node
    Node* next_hop = NULL;
    
    // look up Node object corresponding to curr in neighbors of this node
    deque<Node*> *neighbor_list = GetNeighbors();
    for (deque<Node*>::const_iterator neighbor_itr = neighbor_list->begin(); neighbor_itr != neighbor_list->end(); ++neighbor_itr)
    {
        Node* neighbor = *neighbor_itr;
        if (neighbor->Matches(next_hop_num)) {
            next_hop = neighbor;
            break;
        }
    }

    delete neighbor_list;  // safe to do this (refer to p. 5 of the instructions)

    return next_hop;
}

// Returns a COPY of the routing table (see p. 4 of the project instructions)
Table* LinkState::GetRoutingTable() {
    Table* routing_table = new Table();
    *routing_table = this->routing_table;  // assignment operator overloaded to copy the object
    return routing_table;
}

ostream & LinkState::Print(ostream &os) const { 
    Node::Print(os);
    os << endl;
    return os;
}
