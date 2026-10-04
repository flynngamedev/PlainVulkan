#!/usr/bin/env bash
# Verifies that:
#   1. every Pv:: command declared in runtime/include/pv/pv_runtime.h has
#      exactly one C++ definition somewhere in runtime/src/*.cpp
#   2. the same set of names (and count) appears in the Rust arity table
#      (compiler/src/pv_commands.rs), which codegen uses to validate call
#      sites before emitting C++
#
# Run from anywhere; paths are resolved relative to this script.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

declared=$(grep -oP '^Value \K[A-Za-z_][A-Za-z0-9_]*' runtime/include/pv/pv_runtime.h | sort -u)
defined_raw=$(grep -hoP '^Value \K[A-Za-z_][A-Za-z0-9_]*(?=\()' runtime/src/*.cpp | sort)
# Internal helpers that live in runtime/src alongside the commands but are
# not Pv:: commands themselves, so they have no pv_runtime.h declaration to
# match. Anything added here should be a genuine implementation detail --
# a real command that is merely undeclared is exactly the bug this script
# exists to catch, so the list stays short and explicit rather than becoming
# a pattern that could swallow one.
#   index_get/member_get/make_array -- pv::Value container helpers
#   jsonToValue                     -- wire-format helper, declared in pv_net.h
#   mirrorFor                       -- file-local helper in network_scene.cpp
internal_helpers='^(index_get|member_get|make_array|jsonToValue|mirrorFor)$'
defined=$(echo "$defined_raw" | grep -vE "$internal_helpers" | sort -u)
rust_names=$(grep -oP 'sig!\("\K[A-Za-z_][A-Za-z0-9_]*' compiler/src/pv_commands.rs | sort -u)

fail=0

missing_defs=$(comm -23 <(echo "$declared") <(echo "$defined"))
if [ -n "$missing_defs" ]; then
  echo "MISSING C++ definitions for declared commands:"
  echo "$missing_defs"
  fail=1
fi

extra_defs=$(comm -13 <(echo "$declared") <(echo "$defined"))
if [ -n "$extra_defs" ]; then
  echo "C++ functions defined but not declared in pv_runtime.h (typo?):"
  echo "$extra_defs"
  fail=1
fi

dupes=$(echo "$defined_raw" | grep -vE "$internal_helpers" | uniq -d)
if [ -n "$dupes" ]; then
  echo "Commands with more than one C++ definition:"
  echo "$dupes"
  fail=1
fi

missing_rust=$(comm -23 <(echo "$declared") <(echo "$rust_names"))
if [ -n "$missing_rust" ]; then
  echo "MISSING Rust arity table entries (compiler/src/pv_commands.rs) for:"
  echo "$missing_rust"
  fail=1
fi

extra_rust=$(comm -13 <(echo "$declared") <(echo "$rust_names"))
if [ -n "$extra_rust" ]; then
  echo "Rust arity table has entries with no matching C++ declaration:"
  echo "$extra_rust"
  fail=1
fi

count=$(echo "$declared" | wc -l | tr -d ' ')
if [ "$fail" -eq 0 ]; then
  echo "OK: all $count Pv:: commands have exactly one C++ definition and a matching Rust arity entry."
else
  exit 1
fi
