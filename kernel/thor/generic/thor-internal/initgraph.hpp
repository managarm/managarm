#pragma once

#include <frg/array.hpp>
#include <frg/list.hpp>
#include <thor-internal/debug.hpp>
#include <assert.h>

namespace thor {
namespace initgraph {

inline constexpr bool printDotAnnotations = false;

struct Node;
struct Edge;
struct Engine;

enum class NodeType {
	none,
	stage,
	task
};

struct Edge {
	friend struct Node;
	friend struct Engine;
	friend void realizeEdge(Edge *edge);

	Edge(Node *source, Node *target)
	: source_{source}, target_{target} {
		realizeEdge(this);
	}

	Edge(const Edge &) = delete;

	Edge &operator= (const Edge &) = delete;

private:
	Node *source_;
	Node *target_;

	frg::default_list_hook<Edge> outHook_;
	frg::default_list_hook<Edge> inHook_;
};

struct Node {
	friend struct Engine;
	friend void realizeNode(Node *node);
	friend void realizeEdge(Edge *edge);

	Node(NodeType type, Engine *engine, const char *displayName = nullptr)
	: type_{type}, engine_{engine}, displayName_{displayName} {
		realizeNode(this);
	}

	Node(const Node &) = delete;

	Node &operator= (const Node &) = delete;

protected:
	virtual void activate() { };

	~Node() = default;

private:
	NodeType type_;
	Engine *engine_;

	const char *displayName_;

	frg::intrusive_list<
		Edge,
		frg::locate_member<
			Edge,
			frg::default_list_hook<Edge>,
			&Edge::outHook_
		>
	> outList_;

	frg::intrusive_list<
		Edge,
		frg::locate_member<
			Edge,
			frg::default_list_hook<Edge>,
			&Edge::inHook_
		>
	> inList_;

	frg::default_list_hook<Node> nodesHook_;
	frg::default_list_hook<Node> queueHook_;

	bool done_ = false;
	bool wanted_ = false;

	unsigned int nUnsatisfied = 0;
};

struct Engine {
	friend void realizeNode(Node *node);

	constexpr Engine() = default;

	void run(Node *goal = nullptr) {
		frg::intrusive_list<
			Node,
			frg::locate_member<
				Node,
				frg::default_list_hook<Node>,
				&Node::queueHook_
			>
		> q;

		// First, identify all nodes that we want to run.
		if(goal) {
			if(!goal->wanted_) {
				q.push_back(goal);
				goal->wanted_ = true;
			}

			while(!q.empty())  {
				auto node = q.pop_front();

				// We also want the dependencies of the current node.
				for(auto edge : node->inList_) {
					if(!edge->source_->wanted_) {
						q.push_back(edge->source_);
						edge->source_->wanted_ = true;
					}
				}
			}
		}else{
			// If no goal is defined, we pick all nodes.
			for(auto node : nodes_)
				node->wanted_ = true;
		}

		for(auto node : nodes_) {
			if(!node->wanted_)
				continue;
			// Skip nodes that ran in a previous call to run().
			if(node->done_)
				continue;

			if(!node->nUnsatisfied)
				pending_.push_back(node);
		}

		// Now, run pending nodes until no such nodes remain.
		while(!pending_.empty()) {
			auto current = pending_.pop_front();
			assert(current->wanted_);
			assert(!current->done_);

			if(current->type_ == NodeType::task)
				infoLogger() << "thor: Running task " << current->displayName_
						<< frg::endlog;

			current->activate();
			current->done_ = true;

			if(current->type_ == NodeType::stage)
				infoLogger() << "thor: Reached stage " << current->displayName_
						<< frg::endlog;

			for(auto edge : current->outList_) {
				auto successor = edge->target_;

				assert(successor->nUnsatisfied);
				--successor->nUnsatisfied;
				if(successor->wanted_ && !successor->done_ && !successor->nUnsatisfied)
					pending_.push_back(successor);
			}
		}

		unsigned int nUnreached = 0;
		for(auto node : nodes_) {
			if(!node->wanted_)
				continue;
			if(node->done_)
				continue;
			if(node->type_ == NodeType::stage)
				infoLogger() << "thor: Initialization stage "
						<< node->displayName_ << " could not be reached" << frg::endlog;
			++nUnreached;
		}

		if(nUnreached)
			panicLogger() << "thor: There are " << nUnreached << " initialization nodes"
					" that could not be reached (circular dependencies?)" << frg::endlog;
	}

private:
	frg::intrusive_list<
		Node,
		frg::locate_member<
			Node,
			frg::default_list_hook<Node>,
			&Node::nodesHook_
		>
	> nodes_;

	frg::intrusive_list<
		Node,
		frg::locate_member<
			Node,
			frg::default_list_hook<Node>,
			&Node::queueHook_
		>
	> pending_;
};

inline void realizeNode(Node *node) {
	if(node->type_ == NodeType::stage) {
		infoLogger() << "thor: Registering stage " << node->displayName_
				<< frg::endlog;
	}else if(node->type_ == NodeType::task) {
		infoLogger() << "thor: Registering task " << node->displayName_
				<< frg::endlog;
	}

	node->engine_->nodes_.push_back(node);

	if(printDotAnnotations) {
		if(node->type_ == NodeType::stage) {
			infoLogger() << "thor, initgraph.dot: n" << node
					<< " [label=\"" << node->displayName_ << "\", shape=box];" << frg::endlog;
		}else if(node->type_ == NodeType::task) {
			infoLogger() << "thor, initgraph.dot: n" << node
					<< " [label=\"" << node->displayName_ << "\"];" << frg::endlog;
		}
	}
}

inline void realizeEdge(Edge *edge) {
	edge->source_->outList_.push_back(edge);
	edge->target_->inList_.push_back(edge);
	++edge->target_->nUnsatisfied;

	if(printDotAnnotations)
		infoLogger() << "thor, initgraph.dot: n" << edge->source_
				<< " -> n" << edge->target_ << ";" << frg::endlog;
}

struct Stage final : Node {
	Stage(Engine *engine, const char *displayName)
	: Node{NodeType::stage, engine, displayName} { }
};

template<size_t N>
struct Requires {
	template<typename... Args>
	requires (std::is_convertible_v<Args, Node *> && ...)
	Requires(Args &&... args)
	: array{{args...}} { }

	Requires(const Requires &) = default;

	frg::array<Node *, N> array;
};

template<typename... Args>
Requires(Args &&...) -> Requires<sizeof...(Args)>;

template<size_t N>
struct Entails {
	template<typename... Args>
	Entails(Args &&... args)
	requires (std::is_convertible_v<Args, Node *> && ...)
	: array{{args...}} { }

	frg::array<Node *, N> array;
};

template<typename... Args>
Entails(Args &&...) -> Entails<sizeof...(Args)>;

struct IntoEdgesTo {
	template<typename... Args>
	frg::array<Edge, sizeof...(Args)> operator() (Args &&... args) const {
		return {{{args, target}...}};
	}

	Node *target;
};

struct IntoEdgesFrom {
	template<typename... Args>
	frg::array<Edge, sizeof...(Args)> operator() (Args &&... args) const {
		return {{{source, args}...}};
	}

	Node *source;
};

template<size_t... S, typename T, size_t N, typename I>
auto apply(std::index_sequence<S...>, frg::array<T, N> array, I invocable) {
	return invocable(array[S]...);
}

template< typename F, size_t NR = 0, size_t NE = 0>
struct Task final : Node {
	Task(Engine *engine, const char *displayName, Requires<NR> r, Entails<NE> e, F invocable)
	: Node{NodeType::task, engine, displayName}, invocable_{std::move(invocable)},
			rEdges_{apply(std::make_index_sequence<NR>{}, r.array, IntoEdgesTo{this})},
			eEdges_{apply(std::make_index_sequence<NE>{}, e.array, IntoEdgesFrom{this})} { }

	Task(Engine *engine, const char *displayName, F invocable)
	: Task{engine, displayName, {}, {}, std::move(invocable)} { }

	Task(Engine *engine, const char *displayName, Requires<NR> r, F invocable)
	: Task{engine, displayName, r, {}, std::move(invocable)} { }

	Task(Engine *engine, const char *displayName, Entails<NE> e, F invocable)
	: Task{engine, displayName, {}, e, std::move(invocable)} { }

protected:
	void activate() override {
		invocable_();
	}

private:
	F invocable_;
	frg::array<Edge, NR> rEdges_;
	frg::array<Edge, NE> eEdges_;
};

} } // namespace thor::initgraph
