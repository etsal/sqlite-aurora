# AuroraVFS

SQLite module that implements a SQLite VFS that uses memory regions as files and services operations with direct pointer accesses. Uses the SLS API for synchronization/persistence of memory regions. 

Note: The module must be compiled against a local sqlite source tree, specified in the Makefile.
