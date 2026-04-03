#include "GraphManager.h"

#include <filesystem>

void UndirectedGraphManager::add_connection(const std::string &from, const std::string &to) {
    Vertex u = get_or_add_vertex(from);
    Vertex v = get_or_add_vertex(to);
    boost::add_edge(u, v, graph_);
}

std::optional<Vertex> UndirectedGraphManager::get_vertex(const std::string &vertex) {
    if (name_to_vertex_.find(vertex) != name_to_vertex_.end()) {
        return name_to_vertex_[vertex];
    }

    return std::nullopt;
}

Vertex UndirectedGraphManager::get_or_add_vertex(const std::string &vertex) {
    std::optional<Vertex> v_opt = get_vertex(vertex);
    if (v_opt.has_value()) {
        return v_opt.value();
    }

    Vertex v = boost::add_vertex(vertex, graph_);
    name_to_vertex_[vertex] = v;
    return v;
}

void UndirectedGraphManager::save_graph(const std::string &destination_path) {
    std::filesystem::path path = destination_path;
    if (path.extension() != ".dot") {
        path.replace_extension(".dot");
    }

    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream dot_file(path);

    auto vertex_writer = [&](std::ostream &out, const auto &v) {
        out << "[label=\"" << graph_[v] << "\"]";
    };

    boost::write_graphviz(dot_file, graph_, vertex_writer);
}
