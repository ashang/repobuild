// Copyright 2013
// Author: Christopher Van Arsdale

#include <iostream>
#include <map>
#include <string>
#include <set>
#include <queue>
#include <vector>
#include "common/log/log.h"
#include "common/file/fileutil.h"
#include "common/strings/path.h"
#include "common/util/stl.h"
#include "repobuild/distsource/dist_source.h"
#include "repobuild/env/input.h"
#include "repobuild/nodes/node.h"
#include "repobuild/nodes/allnodes.h"
#include "repobuild/reader/buildfile.h"
#include "repobuild/reader/parser.h"
#include "repobuild/third_party/json/json.h"

using std::map;
using std::set;
using std::string;
using std::queue;
using std::vector;

namespace repobuild {
namespace {
// ParseNode
//  Helper to parse a node given the BUILD file contents, the name of the
//  node (e.g. cc_library, go_library, etc), 
Node* ParseNode(const NodeBuilderSet* builder_set,
                BuildFile* file,
                BuildFileNode* file_node,
                DistSource* dist_source,
                const Input& input,
                const string& key) {
  const Json::Value& value = file_node->object()[key];
  const Json::Value& name = value["name"];

  // Generate a name for this target.
  string node_name;
  if (!name.isNull()) {
    LOG_IF(FATAL, !name.isString()) << "Require string value of \"name\", "
                                    << "found " << name << " in file "
                                    << file->filename();
    node_name = name.asString();
  } else {
    node_name = file->NextName("auto_");
  }

  // Generate the node.
  TargetInfo target(":" + node_name, file->filename());
  Node* node = builder_set->NewNode(key, target, input, dist_source);
  LOG_IF(FATAL, node == NULL) << "Uknown build rule: " << key;
  node->Parse(file, BuildFileNode(value));
  return node;
}

bool UserInputHasTarget(const Input& input, const Node& node) {
  if (input.contains_target(node.target().full_path())) {
    return true;
  }

  // We'll accept required parents of input nodes:
  for (const Node* child : node.dependencies()) {
    if (ContainsValue(child->required_parents(), node.target())) {
      return UserInputHasTarget(input, *child);
    }
  }
  return false;
}

// Graph
//  This does the heavy lifting of parsing a set of dependent build files.
class Graph {
 public:
  Graph(const Input& input,
        const NodeBuilderSet* builder_set,
        DistSource* dist_source)
      : input_(input),
        dist_source_(dist_source),
        builder_set_(builder_set) {
    Parse();
  }

  ~Graph() {
    DeleteValues(&build_files_);
    DeleteValues(&nodes_);
  }

  // Extract
  //  Fills in computed values, now owned by the caller.
  void Extract(vector<Node*>* inputs,
               map<string, Node*>* nodes,
               map<string, BuildFile*>* build_files) {
    *build_files = build_files_; build_files_.clear();
    *inputs = inputs_; inputs_.clear();
    *nodes = nodes_;   nodes_.clear();
  }

 private:
  // Parse
  //  Given an input, this goes and does all of the heavy lifting to read
  //  build files, etc.
  void Parse() {
    // Seed initial targets.
    set<string> queued_targets;
    for (const TargetInfo& info : input_.build_targets()) {
      const string& cleaned = info.full_path();
      if (queued_targets.insert(cleaned).second) {
        to_process_.push(cleaned);
      }
    }

    // Parse our dependency graph using something like BFS.
    set<string> processed_targets;
    while (!to_process_.empty()) {
      const string& key = to_process_.front();
      processed_targets.insert(key);
      ProcessTarget(key);
      to_process_.pop();
    }

    // Get rid of all non-processed nodes (nodes in files that we ignored
    // because they were not on our dependency chain).
    map<string, Node*> copy;
    for (const string& key : processed_targets) {
      copy[key] = nodes_[key];
      nodes_.erase(key);
    }
    DeleteValues(&nodes_);
    swap(nodes_, copy);

    // Now make sure all nodes point to their subnodes.
    for (auto it : nodes_) {
      Node* node = it.second;
      for (const TargetInfo& info : node->dep_targets()) {
        Node* dep = nodes_[info.full_path()];
        CHECK(dep) << "Cannot find: " << info.full_path()
                   << ", which is dependency of " << node->target().full_path();
        node->AddDependencyNode(dep);
      }
    }

    // Figure out which ones came from our input, and save them specially.
    for (auto it : nodes_) {
      Node* node = it.second;
      if (UserInputHasTarget(input_, *node)) {
        inputs_.push_back(node);
      }
    }

    // Now run the post-parse for anyone that needs it.
    for (auto it : nodes_) {
      it.second->PostParse();
    }
  }

  BuildFile* AddFile(const string& filename) {
    // Skip processing if we have done it already.
    if (ContainsKey(build_files_, filename)) {
      return build_files_.find(filename)->second;
    }

    // Initialize our parents (recursive, it calls back into AddFile).
    dist_source_->InitializeForFile(filename, NULL /* ignored */);
    BuildFile* file =  new BuildFile(filename);
    build_files_[filename] = file;
    ProcessParent(file);  // inherit anything we need to from parents.

    // Parse the BUILD into a structured format.
    string filestr = file::ReadFileToStringOrDie(file->filename());
    file->Parse(filestr);

    // Get the dependent files.
    vector<Node*> nodes;
    for (BuildFileNode* node : file->nodes()) {
      LOG_IF(FATAL, !node->object().isObject())
          << "Expected json object (file = " << file->filename() << "): "
          << node->object();
      for (const string& key : node->object().getMemberNames()) {
        if (key == "config" || key == "plugin") {
          ParseSingleNode(file, node, key, &nodes);
        }
      }
    }

    // Parse any BUILD files that our "config" depends on.
    for (const Node* n : nodes) {
      for (const TargetInfo& target : n->pre_parse()) {
        file->MergeDependency(AddFile(target.build_file()));
      }
    }

    // Parse the rest of the elements of the build file.
    for (BuildFileNode* node : file->nodes()) {
      bool expand_plugin = true;
      while (expand_plugin) {
        expand_plugin = false;
        for (const string& key : node->object().getMemberNames()) {
          if (ExpandPlugin(file, node, key)) {
            expand_plugin = true;
            break;
          }
        }
      }

      for (const string& key : node->object().getMemberNames()) {
        if (key != "config" && key != "plugin") {
          ParseSingleNode(file, node,  key, &nodes);
        }
      }
    }

    // Connect any additional dependencies from the build file.
    // TODO(cvanarsdale): We can only have one at the moment, due to how these
    // get added.
    for (const string& additional_dep : file->base_dependencies()) {
      Node* base_dep = nodes_[additional_dep];
      CHECK(base_dep);
      for (Node* node : nodes) {
        if (node->target().full_path() != additional_dep) {
          node->AddDependencyTarget(base_dep->target());
        }
      }
    }
    return file;
  }

  // ExpandTarget
  //  Find all dependencies of a particular node, and enqueue them to be
  //  processed.
  void ExpandTarget(const TargetInfo& target) {
    Node* node = nodes_[target.full_path()];
    LOG_IF(FATAL, node == NULL) << "Could not find target: "
                                << target.full_path();
    for (const TargetInfo& dep : node->dep_targets()) {
      if (already_queued_.insert(dep.full_path()).second) {
        VLOG(1) << "Adding dep: "
                << node->target().full_path()
                << " -> " << dep.full_path();
        to_process_.push(dep.full_path());
      }
    }
    for (const TargetInfo& dep : node->required_parents()) {
      if (already_queued_.insert(dep.full_path()).second) {
        VLOG(1) << "Saw parent request: "
                << node->target().full_path()
                << " -> " << dep.full_path();
        to_process_.push(dep.full_path());
      }
    }
  }

  // ProcessTarget
  //  Given a target string, process the node.
  //   1) Figure out if we have to process the file.
  //   2) If so, parse all nodes in that file.
  //   3) Find all dependencies of the target, and enqueue them to be processed.
  void ProcessTarget(const string& current) {
    std::cout << "Processing: " << current << std::endl;

    // Parse the target.
    TargetInfo target(current);

    // Add the build file if we have not yet processed it.
    AddFile(target.build_file());

    // Expand the target if we managed to find one in that BUILD file.
    ExpandTarget(target);
  }

  void ProcessParent(BuildFile* child) {
    BuildFile* current = child;
    while (true) {
      string current_dir = strings::PathDirname(current->filename());
      if (current_dir == "." || current_dir == input_.root_dir()) {
        break;
      }

      string parent_file = strings::JoinPath(
          strings::JoinPath(current_dir, ".."), "BUILD");
      BuildFile* parent = AddFile(parent_file);
      child->MergeParent(parent);
      current = parent;
    }
  }

  bool ExpandPlugin(BuildFile* file,
                    BuildFileNode* file_node,
                    const string& key) {
    VLOG(2) << "Checking for plugin: " << key;

    // TODO(cvanarsdale): string stuff here is hacky.
    string plugin_target = file->GetKey("plugin:" + key);
    if (plugin_target.empty()) {
      VLOG(2) << "Could not find plugin: " << key;
      return false;
    }
    Node* node = nodes_[plugin_target];
    CHECK(node);
    return node->ExpandBuildFileNode(file, file_node);
  }
  
  void ParseSingleNode(BuildFile* file,
                       BuildFileNode* file_node,
                       const string& key,
                       vector<Node*>* all) {
    Node* node = ParseNode(builder_set_, file, file_node,
                           dist_source_, input_, key);

    VLOG(1) << "Saving node: " << node->target().full_path();
    // Gather all subnodes + this parent node.
    vector<Node*> nodes;
    node->ExtractSubnodes(&nodes);
    nodes.push_back(node);

    // For each node, 
    for (Node* out_node : nodes) {
      const string& target = out_node->target().full_path();
      LOG_IF(FATAL, ContainsKey(nodes_, target))
          << "Duplicate target: " << target;

      // Save the output
      all->push_back(out_node);
      nodes_[target] = out_node;
    }
  }

  // Our inputs
  const Input& input_;
  DistSource* dist_source_;

  // The generated data.
  const NodeBuilderSet* builder_set_;
  map<string, BuildFile*> build_files_;
  map<string, Node*> nodes_;
  vector<Node*> inputs_;  // subset of nodes_.

  // Scratch variables
  set<string> already_queued_;
  queue<string> to_process_;
};
}

Parser::Parser(const NodeBuilderSet* builder_set, DistSource* source)
    : builder_set_(builder_set),
      dist_source_(source) {
}

Parser::~Parser() {
  Reset();
}

void Parser::Parse(const Input& input) {
  Reset();

  Graph graph(input, builder_set_, dist_source_);
  graph.Extract(&input_nodes_, &all_nodes_, &builds_);
  for (auto it : all_nodes_) {
    all_node_vec_.push_back(it.second);
  }
}

void Parser::Reset() {
  input_.reset();
  for (auto it : all_nodes_) {
    delete it.second;
  }
  input_nodes_.clear();
  all_nodes_.clear();
  all_node_vec_.clear();
  for (auto it : builds_) {
    delete it.second;
  }
  builds_.clear();
}

}  // namespace repobuild
