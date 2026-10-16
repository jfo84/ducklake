#include "metadata_manager/sqlite_metadata_manager.hpp"
#include "common/ducklake_util.hpp"
#include "duckdb/main/database.hpp"
#include "storage/ducklake_catalog.hpp"
#include "storage/ducklake_transaction.hpp"

namespace duckdb {

SQLiteMetadataManager::SQLiteMetadataManager(DuckLakeTransaction &transaction) : DuckLakeMetadataManager(transaction) {
}

bool SQLiteMetadataManager::TypeIsNativelySupported(const LogicalType &type) {
	switch (type.id()) {
	// Unnamed composite types are not supported.
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::LIST:
	// SQLite converts IEEE 754 NaN to NULL when storing double values,
	// so FLOAT/DOUBLE must be stored as VARCHAR to preserve NaN through the round-trip
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::VARIANT:
		return false;
	default:
		return true;
	}
}

bool SQLiteMetadataManager::IsRetryableCommitError(const string &message) const {
	auto lower_message = StringUtil::Lower(message);
	StringUtil::Trim(lower_message);
	return StringUtil::EndsWith(lower_message, "database is locked") &&
	       !StringUtil::Contains(lower_message, "failed to execute query \"commit\": database is locked");
}

string SQLiteMetadataManager::GetColumnTypeInternal(const LogicalType &column_type) {
	switch (column_type.id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return "VARCHAR";
	case LogicalTypeId::SQLNULL:
		return "INTEGER";
	default:
		return column_type.ToString();
	}
}

//! The type the stats table declares for a column, since sqlite_query returns every value as VARCHAR
static string StatsColumnType(const string &stat) {
	if (stat == "value_count" || stat == "null_count" || stat == "column_size_bytes") {
		return "BIGINT";
	}
	if (stat == "contains_nan" || stat == "min_is_exact" || stat == "max_is_exact") {
		return "BOOLEAN";
	}
	if (stat == "min_value" || stat == "max_value" || stat == "extra_stats") {
		return "VARCHAR";
	}
	throw InternalException("No declared type for file column stat \"%s\"", stat);
}

//! sqlite_scan pushes no filter into SQLite, so the base CTE copies every table's stats rows into DuckDB on each
//! filtered read. sqlite_query runs the WHERE inside SQLite, in the same transaction.
string SQLiteMetadataManager::GenerateFileColumnStatsCTEBody(const CTERequirement &req, TableIndex table_id,
                                                             const string &metadata_table_prefix) {
	if (metadata_table_prefix != "{METADATA_CATALOG}") {
		return DuckLakeMetadataManager::GenerateFileColumnStatsCTEBody(req, table_id, metadata_table_prefix);
	}
	string native_list = "data_file_id";
	string select_list = "data_file_id::BIGINT AS data_file_id";
	for (const auto &stat : req.referenced_stats) {
		if (stat == "value_count" || stat == "null_count") {
			// every row of a file counts as a value or a NULL, so zero counts of both are unknown
			native_list += StringUtil::Format(
			    ", CASE WHEN value_count = 0 AND null_count = 0 THEN NULL ELSE %s END AS %s", stat, stat);
		} else {
			native_list += ", " + stat;
		}
		select_list += StringUtil::Format(", %s::%s AS %s", stat, StatsColumnType(stat), stat);
	}
	auto native_query =
	    StringUtil::Format("SELECT %s FROM ducklake_file_column_stats WHERE column_id = %d AND table_id = %d",
	                       native_list, req.column_field_index, table_id.index);
	return StringUtil::Format("  SELECT %s\n"
	                          "  FROM sqlite_query({METADATA_CATALOG_NAME_LITERAL}, %s)\n",
	                          select_list, SQLString(native_query));
}

} // namespace duckdb
