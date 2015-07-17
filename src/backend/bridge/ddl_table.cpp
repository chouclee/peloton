/*-------------------------------------------------------------------------
 *
 * ddl_table.cpp
 * file description
 *
 * Copyright(c) 2015, CMU
 *
 * /peloton/src/backend/bridge/ddl_table.cpp
 *
 *-------------------------------------------------------------------------
 */

#include <cassert>
#include <iostream>
#include <vector>

#include "backend/bridge/ddl_table.h"
#include "backend/common/logger.h"
#include "backend/storage/backend_vm.h"
#include "backend/storage/table_factory.h"
#include "backend/storage/database.h"

#include "nodes/pg_list.h"

namespace peloton {
namespace bridge {

//===--------------------------------------------------------------------===//
// Table DDL
//===--------------------------------------------------------------------===//

/**
 * @brief Create table.
 * @param table_name Table name
 * @param column_infos Information about the columns
 * @param schema Schema for the table
 * @return true if we created a table, false otherwise
 */
bool DDLTable::CreateTable(Oid relation_oid,
                           std::string table_name,
                           std::vector<catalog::Column> column_infos,
                           catalog::Schema *schema){

  assert(!table_name.empty());

  Oid database_oid = Bridge::GetCurrentDatabaseOid();
  if(database_oid == INVALID_OID || relation_oid == INVALID_OID)
    return false;

  // Get db oid
  auto& manager = catalog::Manager::GetInstance();
  storage::Database* db = manager.GetDatabaseWithOid(database_oid);

  // Construct our schema from vector of ColumnInfo
  if(schema == NULL)
    schema = new catalog::Schema(column_infos);

  // Build a table from schema
  storage::DataTable *table = storage::TableFactory::GetDataTable(database_oid, relation_oid, schema, table_name);

  db->AddTable(table);

  if(table != nullptr) {
    LOG_INFO("Created table(%u) : %s", relation_oid, table_name.c_str());
    return true;
  }

  return false;
}

/**
 * @brief AlterTable with given AlterTableStmt
 * @param relation_oid relation oid
 * @param Astmt AlterTableStmt
 * @return true if we alter the table successfully, false otherwise
 */
bool DDLTable::AlterTable(Oid relation_oid, AlterTableStmt* Astmt){

  ListCell* lcmd;
  foreach(lcmd, Astmt->cmds)
  {
    AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);

    switch (cmd->subtype){
      //case AT_AddColumn:  /* add column */
      //case AT_DropColumn:  /* drop column */

      case AT_AddConstraint:  /* ADD CONSTRAINT */
      {
        bool status = AddConstraint(relation_oid, (Constraint*) cmd->def);

        if(status == false){
          LOG_WARN("Failed to add constraint");
        }
        break;
      }
      default:
        break;
    }
  }

  LOG_INFO("Alter the table (%u)\n", relation_oid);
  return true;
}

/**
 * @brief Drop table.
 * @param table_oid Table id.
 * @return true if we dropped the table, false otherwise
 */
// FIXME :: Dependencies btw indexes and tables
bool DDLTable::DropTable(Oid table_oid) {

  oid_t database_oid = Bridge::GetCurrentDatabaseOid();

  if(database_oid == InvalidOid || table_oid == InvalidOid) {
    LOG_WARN("Could not drop table :: db oid : %u table oid : %u", database_oid, table_oid);
    return false;
  }

  // Get db with current database oid
  auto& manager = catalog::Manager::GetInstance();
  storage::Database* db = manager.GetDatabaseWithOid(database_oid);

  db->DropTableWithOid(table_oid);

  LOG_INFO("Dropped table with oid : %u\n", table_oid);

  return true;
}

/**
 * @brief Add new constraint to the table
 * @param relation_oid relation oid
 * @param constraint constraint
 * @return true if we add the constraint, false otherwise
 */
bool DDLTable::AddConstraint(Oid relation_oid, Constraint* constraint)
{

  ConstraintType contype = PostgresConstraintTypeToPelotonConstraintType((PostgresConstraintType) constraint->contype);
  std::vector<catalog::ForeignKey> foreign_keys;

  std::string conname;
  if(constraint->conname != NULL){
    conname = constraint->conname;
  }else{
    conname = "";
  }

  // FIXME
  // Create a new constraint
  //catalog::Constraint* new_constraint;


  switch(contype){
    std::cout << "const type : " << ConstraintTypeToString(contype) << std::endl;

    case CONSTRAINT_TYPE_FOREIGN:
    {
      oid_t database_oid = Bridge::GetCurrentDatabaseOid();
      assert(database_oid);

      auto& manager = catalog::Manager::GetInstance();
      storage::Database* db = manager.GetDatabaseWithOid(Bridge::GetCurrentDatabaseOid());

      // PrimaryKey Table
      oid_t PrimaryKeyTableId = db->GetTableWithName(constraint->pktable->relname)->GetOid();

      // Each table column names
      std::vector<std::string> pk_column_names;
      std::vector<std::string> fk_column_names;

      ListCell   *column;
      if(constraint->pk_attrs != NULL && constraint->pk_attrs->length > 0){
        foreach(column, constraint->pk_attrs){
          char* attname = strVal(lfirst(column));
          pk_column_names.push_back(attname);
        }
      }
      if(constraint->fk_attrs != NULL && constraint->fk_attrs->length > 0){
        foreach(column, constraint->fk_attrs){
          char* attname = strVal(lfirst(column));
          fk_column_names.push_back(attname);
        }
      }

      catalog::ForeignKey*foreign_key = new catalog::ForeignKey(PrimaryKeyTableId,
                                                                pk_column_names,
                                                                fk_column_names,
                                                                constraint->fk_upd_action,
                                                                constraint->fk_del_action,
                                                                conname);
      //new_constraint  = new catalog::Constraint(contype, conname);
      foreign_keys.push_back(*foreign_key);

    }
    break;
    default:
      LOG_WARN("Unrecognized constraint type %d\n", (int) contype);
      break;
  }

  // FIXME :
  bool status = SetReferenceTables(foreign_keys, relation_oid);
  if(status == false){
    LOG_WARN("Failed to set reference tables");
  }

  return true;
}

/**
 * @brief Set Reference Tables
 * @param reference table namees  reference table names
 * @param relation_oid relation oid
 * @return true if we set the reference tables, false otherwise
 */
bool DDLTable::SetReferenceTables(std::vector<catalog::ForeignKey>& foreign_keys,
                                  oid_t relation_oid){
  assert(relation_oid);
  oid_t database_oid = Bridge::GetCurrentDatabaseOid();
  assert(database_oid);

  storage::DataTable* current_table = (storage::DataTable*) catalog::Manager::GetInstance().GetTableWithOid(database_oid, relation_oid);

  for(auto foreign_key : foreign_keys) {
    current_table->AddForeignKey(&foreign_key);
  }

  return true;
}


} // namespace bridge
} // namespace peloton
