#include "duckdb/main/capi_internal.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

struct CAPIReplacementScanData : public ReplacementScanData {
	~CAPIReplacementScanData() {
		if (delete_callback) {
			delete_callback(extra_data);
		}
	}

	duckdb_replacement_callback_t callback;
	void *extra_data;
	duckdb_delete_callback_t delete_callback;
};

struct CAPIReplacementScanInfo {
	CAPIReplacementScanInfo(CAPIReplacementScanData *data) : data(data) {
	}

	CAPIReplacementScanData *data;
	string function_name;
	vector<Value> parameters;
	string error;
};

unique_ptr<TableFunctionRef> duckdb_capi_replacement_callback(ClientContext &context, const string &table_name,
                                                              ReplacementScanData *data) {
	auto &scan_data = (CAPIReplacementScanData &)*data;

	CAPIReplacementScanInfo info(&scan_data);
	scan_data.callback((duckdb_replacement_scan_info)&info, table_name.c_str(), scan_data.extra_data);
	if (!info.error.empty()) {
		throw BinderException("Error in replacement scan: %s\n", info.error);
	}
	if (info.function_name.empty()) {
		// no function provided: bail-out
		return nullptr;
	}
	auto table_function = make_unique<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	for (auto &param : info.parameters) {
		children.push_back(make_unique<ConstantExpression>(move(param)));
	}
	table_function->function = make_unique<FunctionExpression>(info.function_name, move(children));
	return table_function;
}

} // namespace duckdb

void duckdb_add_replacement_scan(duckdb_database db, duckdb_replacement_callback_t replacement, void *extra_data,
                                 duckdb_delete_callback_t delete_callback) {
	if (!db || !replacement) {
		return;
	}
	auto wrapper = (duckdb::DatabaseData *)db;
	auto scan_info = duckdb::make_unique<duckdb::CAPIReplacementScanData>();
	scan_info->callback = replacement;
	scan_info->extra_data = extra_data;
	scan_info->delete_callback = delete_callback;

	auto &config = duckdb::DBConfig::GetConfig(*wrapper->database->instance);
	config.replacement_scans.push_back(
	    duckdb::ReplacementScan(duckdb::duckdb_capi_replacement_callback, move(scan_info)));
}

void duckdb_replacement_scan_set_function_name(duckdb_replacement_scan_info info_p, const char *function_name) {
	if (!info_p || !function_name) {
		return;
	}
	auto info = (duckdb::CAPIReplacementScanInfo *)info_p;
	info->function_name = function_name;
}

void duckdb_replacement_scan_add_parameter(duckdb_replacement_scan_info info_p, duckdb_value parameter) {
	if (!info_p || !parameter) {
		return;
	}
	auto info = (duckdb::CAPIReplacementScanInfo *)info_p;
	auto val = (duckdb::Value *)parameter;
	info->parameters.push_back(*val);
}

void duckdb_replacement_scan_set_error(duckdb_replacement_scan_info info_p, const char *error) {
	if (!info_p || !error) {
		return;
	}
	auto info = (duckdb::CAPIReplacementScanInfo *)info_p;
	info->error = error;
}
