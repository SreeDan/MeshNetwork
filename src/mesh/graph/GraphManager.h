#pragma once

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>

typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS, std::string> UndirectedStringGraph;
typedef boost::graph_traits<UndirectedStringGraph>::vertex_descriptor Vertex;

class UndirectedGraphManager {
public:
    UndirectedGraphManager() = default;

    ~UndirectedGraphManager() = default;

    void add_connection(const std::string &from, const std::string &to);

    std::optional<Vertex> get_vertex(const std::string &vertex);

    Vertex get_or_add_vertex(const std::string &vertex);

    void save_graph(const std::string &destination_path);

private:
    UndirectedStringGraph graph_;
    std::map<std::string, Vertex> name_to_vertex_;
};
