#include "distancevector.h"
#include "link.h"
#include <cfloat>    // for max value of double

DistanceVector::DistanceVector(unsigned n, SimulationContext* c, double b, double l) :
    Node(n, c, b, l)
{
    // initialize routing table to contain own distance vector
    map<unsigned, double> my_distance_vector;
    my_distance_vector[number] = 0; // distance to self is zero
    routing_table.SetDistanceVector(number, my_distance_vector); 

    // initialize routing table to include distance vectors for neighbors (any neighbors that are known at the time this node is constructed)
    deque<Node*> *neighbor_list = GetNeighbors();
    for (deque<Node*>::iterator neighbor_itr = neighbor_list->begin(); neighbor_itr != neighbor_list->end(); ++neighbor_itr)
    {
        Node* neighbor = *neighbor_itr;
        map<unsigned, double> neighbor_distance_vector;
        neighbor_distance_vector[neighbor->GetNumber()] = 0; // distance to self is zero
        routing_table.SetDistanceVector(neighbor->GetNumber(), neighbor_distance_vector);
    }

    delete neighbor_list;  // safe to do this (refer to p. 5 of the instructions)
}   


DistanceVector::DistanceVector(const DistanceVector & rhs) :
    Node(rhs)
{
    *this = rhs;
}

DistanceVector & DistanceVector::operator=(const DistanceVector & rhs) {
    Node::operator=(rhs);
    return *this;
}

DistanceVector::~DistanceVector() {}

// Returns true if and only if the distance vector associated with this node changed as a result of the computation
// Uses the Bellman-Ford equation to update own distance vector (see p. 385 of the textbook)
bool DistanceVector::ComputeOwnDistanceVector()
{
    map<unsigned, double> my_distance_vector = routing_table.GetDistanceVector(number);
    map<unsigned, double> old_distance_vector = my_distance_vector; // for comparison at end of calculation to see if my distance vector changed

    // iterate through all nodes in the network known to this node
    for (map<unsigned, double>::iterator dest_itr = my_distance_vector.begin(); dest_itr != my_distance_vector.end(); ++dest_itr)
    {
        unsigned dest_num = dest_itr->first;
        double least_cost = DBL_MAX;
        my_distance_vector[dest_num] = DBL_MAX; // if no path found to dest_num, its cost should be marked as infinity
        
        // special case destination node equals source node
        if (this->Matches(dest_num)) {
            // update distance vector
            my_distance_vector[dest_num] = 0;   // distance to self is zero
            routing_table.SetDistanceVector(number, my_distance_vector);
            continue;   
        }

        // iterate through neighbors to find least cost paths
        deque<Node*> *neighbor_list = GetNeighbors();
        for (deque<Node*>::iterator neighbor_itr = neighbor_list->begin(); neighbor_itr != neighbor_list->end(); ++neighbor_itr)
        {
            Node* neighbor = *neighbor_itr;

            // if distance vector for neighbor does not yet exist, add it
            if (!routing_table.HasDistanceVector(neighbor->GetNumber())) {
                map<unsigned, double> neighbor_distance_vector;
                neighbor_distance_vector[neighbor->GetNumber()] = 0; // distance to self is zero
                routing_table.SetDistanceVector(neighbor->GetNumber(), neighbor_distance_vector);
            }

            // Bellman-Ford equation

            // find cost from source to neighbor
            double cost_from_src_to_neighbor;
            deque<Link*> *link_list = GetOutgoingLinks();
            for (deque<Link*>::iterator link_itr = link_list->begin(); link_itr != link_list->end(); ++link_itr)
            {
                Link* l = *link_itr;
                if (l->Matches(number, neighbor->GetNumber())) { // this link starts at this node and ends at neighbor
                    cost_from_src_to_neighbor = l->GetLatency();
                    break;
                }
            }

            delete link_list;   // safe to do this (refer to p. 5 of the instructions) 
            
            // find cost from neighbor to destination
            map<unsigned, double> neighbor_distance_vector = routing_table.GetDistanceVector(neighbor->GetNumber());
            map<unsigned, double>::const_iterator found_dest = neighbor_distance_vector.find(dest_num);
            if (found_dest == neighbor_distance_vector.end() || neighbor_distance_vector[dest_num] == DBL_MAX) { // the neighbor has no known cost to reach dest
                continue;
            } 
            double cost_from_neighbor_to_dest = neighbor_distance_vector[dest_num];

            // find total cost of path from source to destination through this neighbor, updating distance vector if necessary
            double cost = cost_from_src_to_neighbor + cost_from_neighbor_to_dest;
            if (cost < least_cost) {
                // update distance vector
                my_distance_vector[dest_num] = cost;
                routing_table.SetDistanceVector(number, my_distance_vector);

                least_cost = cost;
            }
        }
        
        delete neighbor_list;  // safe to do this (refer to p. 5 of the instructions)
    }
    
    return my_distance_vector != old_distance_vector;
}

// Sends a routing message containing this node's distance vector to all of its neighbors. Modifies the message to hide costs
// to nodes to which this node routes through the neighbor receiving the routing message (implements poisoned reverse)
void DistanceVector::SendDistanceVectorPoisonedReverse()
{
    // iterate through neighbors
    deque<Node*> *neighbor_list = GetNeighbors();
    for (deque<Node*>::iterator neighbor_itr = neighbor_list->begin(); neighbor_itr != neighbor_list->end(); ++neighbor_itr)
    {
        Node* neighbor = *neighbor_itr;

        // iterate through destinations of distance vector
        map<unsigned, double> my_distance_vector = routing_table.GetDistanceVector(number);
        for (map<unsigned, double>::iterator dest_itr = my_distance_vector.begin(); dest_itr != my_distance_vector.end(); ++dest_itr)
        {
            unsigned dest = dest_itr->first;

            if (neighbor->Matches(dest)) {
                continue;   // do not poison path that ends at node which receives the distance vector
            }

            Node* temp_dest_node = new Node(dest, this->context, 0, 0);  // dummy node representing destination node
            Node* next_hop = GetNextHop(temp_dest_node);
            if (next_hop == NULL)   {   // this node does not know how to get to dest
                continue;   // no need to poison unknown path to node
            }
            delete temp_dest_node;

            if (next_hop->Matches(neighbor)) {  // this node routes through neighbor to get to destination 
                // Poison reverse: tell the neighbor this node does not know any path to destination
                my_distance_vector.erase(dest_itr);
            }
        }

        // pick the appropriate sequence number
        map<unsigned, unsigned>::iterator seq_itr = sent_seq_num.find(neighbor->GetNumber());
        if (seq_itr != sent_seq_num.end()) {    // a routing message has been sent to this neighbor before
            sent_seq_num[neighbor->GetNumber()]++;   // increment sequence number
        } else {    // no routing message has been sent from this node to this neighbor before
            sent_seq_num[neighbor->GetNumber()] = 0;    // initialize sequence number to 0
        }

        unsigned seq_num = sent_seq_num[neighbor->GetNumber()];

        // send my distance vector to neighbor
        SendToNeighbor(neighbor, new RoutingMessage(number, seq_num, my_distance_vector));
    }

    delete neighbor_list;  // safe to do this (refer to p. 5 of the instructions)
}

// Recomputes this node's distance vector based on the changed link l. Sends routing messages to neighbors if this node's distance vector changes as a result of the link update
// If this link creates a new neighbor for this node, add this neighbor to this node's distance vector to compute least costs for that neighbor
void DistanceVector::LinkHasBeenUpdated(Link* l) {
    cerr << *this << ": Link Update: " << *l << endl;

    deque<Node*> *neighbor_list = GetNeighbors();
    for (deque<Node*>::iterator neighbor_itr = neighbor_list->begin(); neighbor_itr != neighbor_list->end(); ++neighbor_itr)
    {
        Node* neighbor = *neighbor_itr;

        if (neighbor->Matches(l->GetDest())) {
            // if my distance vector does not yet contain neighbor, add neighbor
            map<unsigned, double> my_distance_vector = routing_table.GetDistanceVector(number);
            map<unsigned, double>::iterator found_neighbor_itr = my_distance_vector.find(neighbor->GetNumber());
            if (found_neighbor_itr == my_distance_vector.end()) {   // my distance vector does not yet contain neighbor
                my_distance_vector[neighbor->GetNumber()] = DBL_MAX;
                routing_table.SetDistanceVector(number, my_distance_vector);
            }
            break;
        }
    }

    delete neighbor_list;  // safe to do this (refer to p. 5 of the instructions)
    
    bool my_distance_vector_changed = ComputeOwnDistanceVector();   // update this node's distance vector based on new cost to neighbor

    // Send routing message to all neighbors if and only if change in link cost induced change in this node's distance vector
    if (my_distance_vector_changed) {
        SendDistanceVectorPoisonedReverse();
    }
}

void DistanceVector::ProcessIncomingRoutingMessage(RoutingMessage *m) {
    cerr << *this << " got a routing message: " << *m << endl;

    unsigned src_node = m->from_node;

    // Detect expired routing messages and ignore them. Otherwise, update received sequence number
    map<unsigned, unsigned>::iterator seq_itr = received_seq_num.find(src_node);
    if (seq_itr != received_seq_num.end()) {    // already received at least one routing message from this neighbor
        if (m->seq_num <= seq_itr->second) {    // this routing message has either already been seen or is out of date
            return; // do not process the routing message
        } else {    // the routing message carries new information 
            received_seq_num[src_node] = m->seq_num;    // update received sequence number
        }
    } else {    // first received routing message from this neighbor
        received_seq_num[src_node] = m->seq_num;    // update received sequence number
    }

    // update neighbor distance vector
    map<unsigned, double> incoming_distance_vector = m->distance_vector;
    routing_table.SetDistanceVector(src_node, incoming_distance_vector);

    bool my_distance_vector_changed = false;

    // discover nodes from the incoming distance vector and add them to this node's distance vector if necessary
    map<unsigned, double> my_distance_vector = routing_table.GetDistanceVector(number);
    for (map<unsigned, double>::const_iterator dest_itr = incoming_distance_vector.begin(); dest_itr != incoming_distance_vector.end(); ++dest_itr)
    {
        map<unsigned, double>::const_iterator found_dest = my_distance_vector.find(dest_itr->first);
        if (found_dest == my_distance_vector.end()) {   // the destination node is not yet in my distance vector
            my_distance_vector.insert(pair<unsigned, double>(dest_itr->first, DBL_MAX));    // set the least cost to DBL_MAX temporarily; this will be overwritten when this node's distance vector is recomputed
            routing_table.SetDistanceVector(number, my_distance_vector);
            my_distance_vector_changed = true;
        } 
    }

    // recalculate this node's distance vector
    if (ComputeOwnDistanceVector()) {
        my_distance_vector_changed = true;
    }

    // send routing message to all neighbors if and only if distance vector changed
    if (my_distance_vector_changed) {
        SendDistanceVectorPoisonedReverse();
    }
}

// No timeout functionality is needed for distance vector algorithm
void DistanceVector::TimeOut() {
    cerr << *this << " got a timeout: (ignored)" << endl;
}

// Returns the next hop on the least cost path from this node to destination. Refers to this node's distance vector only. Based on Bellman-Ford equation
Node* DistanceVector::GetNextHop(Node *destination) { 
    unsigned dest_node = (*destination).GetNumber();

    if (dest_node == number) {  // special case destination is this node; note that this node will not be included in its list of neighbors
        return this;
    }

    Node* next_hop_node = NULL;
    double least_cost = DBL_MAX;

    map<unsigned, double> my_distance_vector = routing_table.GetDistanceVector(number);

    // look through neighbors for next hop
    deque<Node*> *neighbor_list = GetNeighbors();
    for (deque<Node*>::iterator neighbor_itr = neighbor_list->begin(); neighbor_itr != neighbor_list->end(); ++neighbor_itr) 
    {
        Node* neighbor = *neighbor_itr;

        // find cost from source to neighbor
        double cost_from_src_to_neighbor;
        deque<Link*> *link_list = GetOutgoingLinks();
        for (deque<Link*>::iterator link_itr = link_list->begin(); link_itr != link_list->end(); ++link_itr)
        {
            Link* l = *link_itr;
            if (l->Matches(number, neighbor->GetNumber())) { // this link starts at this node and ends at neighbor
                cost_from_src_to_neighbor = l->GetLatency();
                break;
            }
        }

        delete link_list;  // safe to do this (refer to p. 5 of the instructions)

        // find cost from neighbor to destination
        map<unsigned, double> neighbor_distance_vector = routing_table.GetDistanceVector(neighbor->GetNumber());
        map<unsigned, double>::const_iterator found_dest = neighbor_distance_vector.find(dest_node);
        if (found_dest == neighbor_distance_vector.end() || neighbor_distance_vector[dest_node] == DBL_MAX) { // the neighbor has no known cost to reach dest
            continue;   // this neighbor does not know how to get to destination, so this neighbor cannot be the next hop
        } 
        double cost_from_neighbor_to_dest = neighbor_distance_vector[dest_node];

        // find total cost of path from source to destination through this neighbor, updating distance vector if necessary
        double cost = cost_from_src_to_neighbor + cost_from_neighbor_to_dest;
        if (cost < least_cost) {   // new potential next hop
            next_hop_node = *neighbor_itr;
            least_cost = cost;
        }
    }

    delete neighbor_list;  // safe to do this (refer to p. 5 of the instructions)

    return next_hop_node;
}

// Returns a COPY of the routing table (see p. 4 of the project instructions)
Table* DistanceVector::GetRoutingTable() {
    Table* routing_table = new Table();
    *routing_table = this->routing_table;  // assignment operator overloaded to copy the object
    return routing_table;
}

ostream & DistanceVector::Print(ostream &os) const { 
    Node::Print(os);
    os << endl;

    return os;
}
