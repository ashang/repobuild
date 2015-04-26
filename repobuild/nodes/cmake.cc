 // Copyright 2013
// Author: Christopher Van Arsdale

#include <map>
#include <string>
#include <set>
#include <iterator>
#include <vector>
#include "common/log/log.h"
#include "common/strings/path.h"
#include "repobuild/env/input.h"
#include "repobuild/nodes/cmake.h"
#include "repobuild/nodes/gen_sh.h"
#include "repobuild/nodes/make.h"
#include "repobuild/reader/buildfile.h"

using std::map;
using std::vector;
using std::string;

namespace repobuild {

void CmakeNode::Parse(BuildFile* file, const BuildFileNode& input) {
  Node::Parse(file, input);

  // CMakeLists.txt file directory
  string cmake_dir =
      current_reader()->ParseSingleDirectory("cmake_dir");
  if (cmake_dir.empty()) {
    cmake_dir = target().dir();
  }
  cmake_dir = strings::JoinPath("$ROOT_DIR", cmake_dir);

  // configure_env
  vector<string> cmake_envs;
  current_reader()->ParseRepeatedString("cmake_env", &cmake_envs);

  // cmake_args
  vector<string> cmake_args;
  current_reader()->ParseRepeatedString("cmake_args", true, &cmake_args);

  // Generate the output files.
  GenShNode* gen = NewSubNodeWithCurrentDeps<GenShNode>(file);
  gen->SetMakeName("Cmake");

  // Users are allowed to specify custom env arg overrides.
  string user_env;
  if (!cmake_envs.empty()) {
    for (const string& it : cmake_envs) {
      user_env.append(" " + it);
    }
    user_env.append("; ");
  }

  // Actual cmake command  ------
  string build_setup =
      "BASE=" + cmake_dir + "; "
      "DEST_DIR=$GEN_DIR; "
      "mkdir -p $DEST_DIR/build; "
      "STAGING=$DEST_DIR/.staging; "
      "cd $GEN_DIR/build";
  string build_env = user_env +"CC=$CC CXX=$CXX ";
  string cmake_cmd =
      "cmake -DCMAKE_INSTALL_PREFIX=. -B . $BASE "
      "-DCMAKE_CXX_FLAGS=\"$BASIC_CXXFLAGS $USER_CXXFLAGS\" "
      "-DCMAKE_C_FLAGS=\"$BASIC_CFLAGS $USER_CFLAGS\"";
  for (const string& it : cmake_args) {
    cmake_cmd.append(" " + it);
  }
  vector<Resource> input_files, output_files;
  gen->Set(build_setup + "; " + build_env + " " + cmake_cmd,
           "",  // clean
           input_files,
           output_files);

  // Make output --------------------------
  string preinstall_cmd = build_setup;
  string postinstall_cmd =
      "(if [ -d \"$STAGING/$BASE\" ]; then"
      " (for f in $(ls -d $STAGING/$BASE/*); do"
      "  rm -rf $DEST_DIR/$(basename \"$f\"); mv $f $DEST_DIR || exit 1;"
      " done) &&"
      " rm -rf $STAGING; "
      "elif [ -d \"$STAGING/$GEN_DIR\" ]; then"
      " (for f in $(ls -d $STAGING/$GEN_DIR/*); do"
      "  rm -rf $DEST_DIR/$(basename \"$f\"); mv $f $DEST_DIR || exit 1;"
      " done) &&"
      " rm -rf $STAGING; "
      " else echo -n ''; "
      "fi)";
  MakeNode* make = NewSubNode<MakeNode>(file);
  make->AddDependencyTarget(gen->target());
  make->ParseWithOptions(file, input,
                         preinstall_cmd,
                         "$STAGING",
                         postinstall_cmd);
}

}  // namespace repobuild
