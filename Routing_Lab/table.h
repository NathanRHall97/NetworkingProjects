#ifndef _table
#define _table

#include <iostream>
#include <map>

using namespace std;

struct TopoLink {
    TopoLink(): cost(-1), age(0) {}

    TopoLink(const TopoLink & rhs) {
        *this = rhs;
    }

    TopoLink & operator=(const TopoLink & rhs) {
        this->cost = rhs.cost;
        this->age = rhs.age;

        return *this;
    }

    double cost;    // cost of TopoLink should equal the latency of the corresponding Link (which is of type double)
    int age;
};

class Table {
    private:
        #if defined(DISTANCEVECTOR)
        map<unsigned, map<unsigned, double> > distance_vectors;   // distance_vectors[src][dest] is the least cost from src node to dest node
        #endif

        #if defined(LINKSTATE)
        map <unsigned, map<unsigned, TopoLink> > topo;    // internal representation of network topology -- topo[src][dest] is the (one-way) TopoLink from src to dest
        #endif
    public:
        Table();
        Table(const Table &);
        Table & operator=(const Table &);

        ostream & Print(ostream &os) const;

        // Anything else you need
        #if defined(DISTANCEVECTOR)
        map<unsigned, double> GetDistanceVector(unsigned src_node) const;
        void SetDistanceVector(unsigned src_node, map<unsigned, double> distance_vector);
        bool HasDistanceVector(unsigned src_node) const;
        #endif

        #if defined(LINKSTATE)
        map <unsigned, map<unsigned, TopoLink> > GetTopo() const;
        TopoLink GetTopoLink(unsigned src, unsigned dest) const;
        void SetTopoLink(unsigned src, unsigned dest, TopoLink link);
        bool HasTopoLink(unsigned src, unsigned dest) const;
        map<unsigned, TopoLink> GetOutgoingTopoLinks(unsigned src) const;
        bool HasOutgoingTopoLinks(unsigned src) const;
        #endif
};

inline ostream & operator<<(ostream &os, const Table & t) { return t.Print(os);}


#endif
