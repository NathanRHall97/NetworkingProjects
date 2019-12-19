#ifndef _distancevector
#define _distancevector

#include "node.h"

class DistanceVector: public Node {
    private:
        // Anything you need in addition to Node members
        // track sequence numbers to detect reception of out of date RoutingMessages
        map<unsigned, unsigned> received_seq_num;
        map<unsigned, unsigned> sent_seq_num;

        bool ComputeOwnDistanceVector();
        void SendDistanceVectorPoisonedReverse();

    public:
        DistanceVector(unsigned, SimulationContext* , double, double);
        DistanceVector(const DistanceVector &);
        DistanceVector & operator=(const DistanceVector &);
        ~DistanceVector();

        // Inherited from Node
        void LinkHasBeenUpdated(Link *l);
        void ProcessIncomingRoutingMessage(RoutingMessage *m);
        void TimeOut();
        Node* GetNextHop(Node* destination);
        Table* GetRoutingTable();
        ostream & Print(ostream & os) const;

        // Anything else
};

inline ostream & operator<<(ostream & os, const DistanceVector & n) {
    return n.Print(os);
}

#endif
