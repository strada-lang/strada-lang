# Makefile for Strada DBI Library
#
# Supports SQLite (default), MySQL, and PostgreSQL
# Enable additional drivers with: make MYSQL=1 POSTGRES=1

CC = gcc
CFLAGS = -shared -fPIC -Wall -Wextra -O2
RUNTIME_INC = -I../../runtime

# SQLite is always included
SQLITE_FLAGS = -DHAVE_SQLITE3
SQLITE_LIBS = -lsqlite3

# Optional MySQL support
ifdef MYSQL
MYSQL_FLAGS = -DHAVE_MYSQL $(shell mysql_config --cflags 2>/dev/null)
MYSQL_LIBS = $(shell mysql_config --libs 2>/dev/null)
endif

# Optional PostgreSQL support
ifdef POSTGRES
PG_FLAGS = -DHAVE_POSTGRES $(shell pkg-config --cflags libpq 2>/dev/null)
PG_LIBS = $(shell pkg-config --libs libpq 2>/dev/null)
endif

ALL_FLAGS = $(CFLAGS) $(RUNTIME_INC) $(SQLITE_FLAGS) $(MYSQL_FLAGS) $(PG_FLAGS)
ALL_LIBS = $(SQLITE_LIBS) $(MYSQL_LIBS) $(PG_LIBS)

TARGET = libstrada_dbi.so

all: $(TARGET)

$(TARGET): strada_dbi.c strada_dbi.h
	$(CC) $(ALL_FLAGS) -o $@ strada_dbi.c $(ALL_LIBS)

# Build with all drivers
all-drivers:
	$(MAKE) MYSQL=1 POSTGRES=1

# SQLite only (default)
sqlite-only:
	$(MAKE)

# Test if dependencies are available
check-deps:
	@echo "Checking dependencies..."
	@echo -n "SQLite3: " && (pkg-config --exists sqlite3 && echo "OK") || echo "MISSING"
	@echo -n "MySQL: " && (mysql_config --version 2>/dev/null && echo "OK") || echo "NOT FOUND"
	@echo -n "PostgreSQL: " && (pkg-config --exists libpq && echo "OK") || echo "NOT FOUND"

clean:
	rm -f $(TARGET) *.o

install: $(TARGET)
	mkdir -p ../../build/lib
	cp $(TARGET) ../../build/lib/

.PHONY: all all-drivers sqlite-only check-deps clean install
