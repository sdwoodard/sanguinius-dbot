#!/bin/bash

# Define usage function
usage() {
  echo "Usage: $0 [clean] [build]"
  exit 1
}

# Define variables
clean=false
build=false

# Iterate over arguments and store them
for arg in "$@"; do
  case $arg in
    clean)
      clean=true
      ;;
    build)
      build=true
      ;;
    *)
      usage
      ;;
  esac
done

# Clean build artifacts if specified
if [ "$clean" = true ]; then
  echo "Cleaning build artifacts..."
  rm -rf CMakeCache.txt
  rm -rf *.cmake
  rm -rf Makefile
  rm -rf CMakeFiles
  rm -rf ../bin/*
fi

# Build project if specified
if [ "$build" = true ]; then
  echo "Generating build files with CMake..."
  cmake .
  echo "Building project..."
  make
fi

# Print usage message if no arguments were specified
if [ "$clean" = false ] && [ "$build" = false ]; then
  usage
fi
