#!/bin/sh

set -e

find src -name '*.[ch]' -o -name '*.cc' | sort | while read srcfile; do
  clang-format -style='{
    BasedOnStyle: Google,
    AllowShortIfStatementsOnASingleLine: false,
    AllowShortLoopsOnASingleLine: false,
    AllowShortFunctionsOnASingleLine: None,
    DerivePointerBinding: false,
    PointerAlignment: Right,
  }' "${srcfile}" > clang-format-tmp
  if ! cmp -s clang-format-tmp "${srcfile}"; then
    echo "=> Style fixing ${srcfile}"
    mv clang-format-tmp "${srcfile}"
  fi
  rm -f clang-format-tmp
done
