/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef __DATASTORE_H__
#define __DATASTORE_H__

#include "config.h"

/* Generic data store based on sqlite3 */

#include <memory>
#include <mutex>
#include <string>

#include <stdio.h>
#include <sqlite3.h>

#include "globalregistry.h"

/* Kismet Datastores
 *
 * Each data store is an independent database file.
 *
 * Each database contains one core table which contains information about
 * the primary module manipulating state, the database version, and the kismet
 * version which last accessed the file.
 *
 * Database versions are monotonically incrementing numbers; a module is
 * expected to detect when the codebase is newer than the database file and
 * perform any upgrades necessary.
 *
 * Providers of datastore objects must subclass the datastore and implement an
 * API to abstract the internal SQL commands; callers should not be expected to
 * implement SQL directly.
 *
 */

class Datastore : public LifetimeGlobal {
    // Subclasses must implement a public builder, typically one which returns a
    // shared pointer instance

private:
    Datastore(GlobalRegistry *in_globalreg);

public:
    virtual ~Datastore();

    virtual unsigned int get_db_version();

    // Initialize a new database
    virtual unsigned int initialize_db() = 0;

    // Upgrade a database which doesn't match our version
    virtual unsigned int upgrade_db(unsigned int version) = 0;

private:
    GlobalRegistry *globalreg;

    // Module name and target version, filled in by subclasses during initialization
    std::string ds_module_name;
    unsigned int ds_target_version;

    std::recursive_timed_mutex ds_mutex;

    sqlite3 *db;
};


#endif


