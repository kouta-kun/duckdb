#include "duckdb/execution/operator/aggregate/physical_ungrouped_aggregate.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/aggregate/aggregate_object.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/execution/radix_partitioned_hashtable.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/algorithm.hpp"
#include <functional>
#include "duckdb/execution/operator/aggregate/distinct_aggregate_data.hpp"

namespace duckdb {

PhysicalUngroupedAggregate::PhysicalUngroupedAggregate(vector<LogicalType> types,
                                                       vector<unique_ptr<Expression>> expressions,
                                                       idx_t estimated_cardinality)
    : PhysicalOperator(PhysicalOperatorType::UNGROUPED_AGGREGATE, move(types), estimated_cardinality),
      aggregates(move(expressions)) {
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
struct AggregateState {
	explicit AggregateState(const vector<unique_ptr<Expression>> &aggregate_expressions) {
		for (auto &aggregate : aggregate_expressions) {
			D_ASSERT(aggregate->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
			auto &aggr = (BoundAggregateExpression &)*aggregate;
			auto state = unique_ptr<data_t[]>(new data_t[aggr.function.state_size()]);
			aggr.function.initialize(state.get());
			aggregates.push_back(move(state));
			destructors.push_back(aggr.function.destructor);
#ifdef DEBUG
			counts.push_back(0);
#endif
		}
	}
	~AggregateState() {
		D_ASSERT(destructors.size() == aggregates.size());
		for (idx_t i = 0; i < destructors.size(); i++) {
			if (!destructors[i]) {
				continue;
			}
			Vector state_vector(Value::POINTER((uintptr_t)aggregates[i].get()));
			state_vector.SetVectorType(VectorType::FLAT_VECTOR);

			destructors[i](state_vector, 1);
		}
	}

	void Move(AggregateState &other) {
		other.aggregates = move(aggregates);
		other.destructors = move(destructors);
	}

	//! The aggregate values
	vector<unique_ptr<data_t[]>> aggregates;
	//! The destructors
	vector<aggregate_destructor_t> destructors;
	//! Counts (used for verification)
	vector<idx_t> counts;
};

class UngroupedAggregateGlobalState : public GlobalSinkState {
public:
	UngroupedAggregateGlobalState(Allocator &allocator, const vector<unique_ptr<Expression>> &aggregates,
	                              ClientContext &client)
	    : state(aggregates), finished(false) {

		vector<idx_t> indices;
		// Determine if there are distinct aggregates
		for (idx_t i = 0; i < aggregates.size(); i++) {
			auto &aggr = (BoundAggregateExpression &)*(aggregates[i]);
			if (!aggr.distinct) {
				continue;
			}
			indices.push_back(i);
		}
		//! None of the aggregates are distinct
		if (indices.empty()) {
			return;
		}

		distinct_data = make_unique<DistinctAggregateData>(allocator, aggregates, move(indices), client);
	}

	//! The lock for updating the global aggregate state
	mutex lock;
	//! The global aggregate state
	AggregateState state;
	//! Whether or not the aggregate is finished
	bool finished;
	//! The data related to the distinct aggregates (if there are any)
	unique_ptr<DistinctAggregateData> distinct_data;
};

class UngroupedAggregateLocalState : public LocalSinkState {
public:
	UngroupedAggregateLocalState(Allocator &allocator, const vector<unique_ptr<Expression>> &aggregates,
	                             const vector<LogicalType> &child_types, GlobalSinkState &gstate_p,
	                             ExecutionContext &context)
	    : state(aggregates), child_executor(allocator), payload_chunk(), filter_set() {
		auto &gstate = (UngroupedAggregateGlobalState &)gstate_p;

		InitializeDistinctAggregates(gstate, context);

		vector<LogicalType> payload_types;
		vector<AggregateObject> aggregate_objects;
		for (auto &aggregate : aggregates) {
			D_ASSERT(aggregate->GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE);
			auto &aggr = (BoundAggregateExpression &)*aggregate;
			// initialize the payload chunk
			for (auto &child : aggr.children) {
				payload_types.push_back(child->return_type);
				child_executor.AddExpression(*child);
			}
			aggregate_objects.emplace_back(&aggr);
		}
		if (!payload_types.empty()) { // for select count(*) from t; there is no payload at all
			payload_chunk.Initialize(allocator, payload_types);
		}
		filter_set.Initialize(allocator, aggregate_objects, child_types);
	}

	//! The local aggregate state
	AggregateState state;
	//! The executor
	ExpressionExecutor child_executor;
	//! The payload chunk, containing all the Vectors for the aggregates
	DataChunk payload_chunk;
	//! Aggregate filter data set
	AggregateFilterDataSet filter_set;
	//! The local sink states of the distinct aggregates hash tables
	vector<unique_ptr<LocalSinkState>> radix_states;

public:
	void Reset() {
		payload_chunk.Reset();
	}
	void InitializeDistinctAggregates(const UngroupedAggregateGlobalState &gstate, ExecutionContext &context) {

		if (!gstate.distinct_data) {
			return;
		}
		auto &data = *gstate.distinct_data;
		auto &distinct_indices = data.Indices();
		if (distinct_indices.empty()) {
			// No distinct aggregates
			return;
		}
		D_ASSERT(!data.radix_tables.empty());

		const idx_t aggregate_cnt = data.radix_tables.size();
		radix_states.resize(aggregate_cnt);

		for (auto &idx : distinct_indices) {
			idx_t table_idx = data.table_map[idx];
			if (data.radix_tables[table_idx] == nullptr) {
				// This aggregate has identical input as another aggregate, so no table is created for it
				continue;
			}
			auto &radix_table = *data.radix_tables[table_idx];
			radix_states[table_idx] = radix_table.GetLocalSinkState(context);
		}
	}
};

unique_ptr<GlobalSinkState> PhysicalUngroupedAggregate::GetGlobalSinkState(ClientContext &context) const {
	return make_unique<UngroupedAggregateGlobalState>(Allocator::Get(context), aggregates, context);
}

unique_ptr<LocalSinkState> PhysicalUngroupedAggregate::GetLocalSinkState(ExecutionContext &context) const {
	D_ASSERT(sink_state);
	auto &gstate = *sink_state;
	return make_unique<UngroupedAggregateLocalState>(Allocator::Get(context.client), aggregates,
	                                                 children[0]->GetTypes(), gstate, context);
}

void PhysicalUngroupedAggregate::SinkDistinct(ExecutionContext &context, GlobalSinkState &state, LocalSinkState &lstate,
                                              DataChunk &input) const {
	auto &sink = (UngroupedAggregateLocalState &)lstate;
	auto &global_sink = (UngroupedAggregateGlobalState &)state;
	D_ASSERT(global_sink.distinct_data);
	auto &distinct_aggregate_data = *global_sink.distinct_data;
	auto &distinct_indices = distinct_aggregate_data.Indices();
	for (auto &idx : distinct_indices) {
		auto &aggregate = (BoundAggregateExpression &)*aggregates[idx];

		idx_t table_idx = distinct_aggregate_data.table_map[idx];
		if (!distinct_aggregate_data.radix_tables[table_idx]) {
			continue;
		}
		D_ASSERT(distinct_aggregate_data.radix_tables[table_idx]);
		auto &radix_table = *distinct_aggregate_data.radix_tables[table_idx];
		auto &radix_global_sink = *distinct_aggregate_data.radix_states[table_idx];
		auto &radix_local_sink = *sink.radix_states[table_idx];

		if (aggregate.filter) {
			// Apply the filter before inserting into the hashtable
			auto &filtered_data = sink.filter_set.GetFilterData(idx);
			idx_t count = filtered_data.ApplyFilter(input);
			filtered_data.filtered_payload.SetCardinality(count);

			radix_table.Sink(context, radix_global_sink, radix_local_sink, filtered_data.filtered_payload,
			                 filtered_data.filtered_payload);
		} else {
			radix_table.Sink(context, radix_global_sink, radix_local_sink, input, input);
		}
	}
}

SinkResultType PhysicalUngroupedAggregate::Sink(ExecutionContext &context, GlobalSinkState &state,
                                                LocalSinkState &lstate, DataChunk &input) const {
	auto &sink = (UngroupedAggregateLocalState &)lstate;
	auto &gstate = (UngroupedAggregateGlobalState &)state;

	// perform the aggregation inside the local state
	sink.Reset();

	if (gstate.distinct_data) {
		SinkDistinct(context, state, lstate, input);
	}

	DataChunk &payload_chunk = sink.payload_chunk;

	idx_t payload_idx = 0;
	idx_t next_payload_idx = 0;

	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		auto &aggregate = (BoundAggregateExpression &)*aggregates[aggr_idx];

		payload_idx = next_payload_idx;
		next_payload_idx = payload_idx + aggregate.children.size();

		if (aggregate.distinct) {
			continue;
		}

		idx_t payload_cnt = 0;
		// resolve the filter (if any)
		if (aggregate.filter) {
			auto &filtered_data = sink.filter_set.GetFilterData(aggr_idx);
			auto count = filtered_data.ApplyFilter(input);

			sink.child_executor.SetChunk(filtered_data.filtered_payload);
			payload_chunk.SetCardinality(count);
		} else {
			sink.child_executor.SetChunk(input);
			payload_chunk.SetCardinality(input);
		}

#ifdef DEBUG
		sink.state.counts[aggr_idx] += payload_chunk.size();
#endif

		// resolve the child expressions of the aggregate (if any)
		for (idx_t i = 0; i < aggregate.children.size(); ++i) {
			sink.child_executor.ExecuteExpression(payload_idx + payload_cnt,
			                                      payload_chunk.data[payload_idx + payload_cnt]);
			payload_cnt++;
		}

		auto start_of_input = payload_cnt == 0 ? nullptr : &payload_chunk.data[payload_idx];
		AggregateInputData aggr_input_data(aggregate.bind_info.get(), Allocator::DefaultAllocator());
		aggregate.function.simple_update(start_of_input, aggr_input_data, payload_cnt,
		                                 sink.state.aggregates[aggr_idx].get(), payload_chunk.size());
	}
	return SinkResultType::NEED_MORE_INPUT;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//

void PhysicalUngroupedAggregate::CombineDistinct(ExecutionContext &context, GlobalSinkState &state,
                                                 LocalSinkState &lstate) const {
	auto &global_sink = (UngroupedAggregateGlobalState &)state;
	auto &source = (UngroupedAggregateLocalState &)lstate;
	auto &distinct_aggregate_data = global_sink.distinct_data;

	if (!distinct_aggregate_data) {
		return;
	}
	auto table_count = distinct_aggregate_data->radix_tables.size();
	for (idx_t table_idx = 0; table_idx < table_count; table_idx++) {
		D_ASSERT(distinct_aggregate_data->radix_tables[table_idx]);
		auto &radix_table = *distinct_aggregate_data->radix_tables[table_idx];
		auto &radix_global_sink = *distinct_aggregate_data->radix_states[table_idx];
		auto &radix_local_sink = *source.radix_states[table_idx];

		radix_table.Combine(context, radix_global_sink, radix_local_sink);
	}
}

void PhysicalUngroupedAggregate::Combine(ExecutionContext &context, GlobalSinkState &state,
                                         LocalSinkState &lstate) const {
	auto &gstate = (UngroupedAggregateGlobalState &)state;
	auto &source = (UngroupedAggregateLocalState &)lstate;
	D_ASSERT(!gstate.finished);

	// finalize: combine the local state into the global state
	// all aggregates are combinable: we might be doing a parallel aggregate
	// use the combine method to combine the partial aggregates
	lock_guard<mutex> glock(gstate.lock);

	CombineDistinct(context, state, lstate);

	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		auto &aggregate = (BoundAggregateExpression &)*aggregates[aggr_idx];

		if (aggregate.distinct) {
			continue;
		}

		Vector source_state(Value::POINTER((uintptr_t)source.state.aggregates[aggr_idx].get()));
		Vector dest_state(Value::POINTER((uintptr_t)gstate.state.aggregates[aggr_idx].get()));

		AggregateInputData aggr_input_data(aggregate.bind_info.get(), Allocator::DefaultAllocator());
		aggregate.function.combine(source_state, dest_state, aggr_input_data, 1);
#ifdef DEBUG
		gstate.state.counts[aggr_idx] += source.state.counts[aggr_idx];
#endif
	}

	auto &client_profiler = QueryProfiler::Get(context.client);
	context.thread.profiler.Flush(this, &source.child_executor, "child_executor", 0);
	client_profiler.Flush(context.thread.profiler);
}

class DistinctAggregateFinalizeTask : public ExecutorTask {
public:
	DistinctAggregateFinalizeTask(Executor &executor, shared_ptr<Event> event_p, UngroupedAggregateGlobalState &state_p,
	                              ClientContext &context, const PhysicalUngroupedAggregate &op)
	    : ExecutorTask(executor), event(move(event_p)), gstate(state_p), context(context), op(op) {
	}

	void AggregateDistinct() {
		D_ASSERT(gstate.distinct_data);
		auto &aggregates = op.aggregates;
		auto &distinct_aggregate_data = *gstate.distinct_data;
		auto &payload_chunk = distinct_aggregate_data.payload_chunk;

		ThreadContext temp_thread_context(context);
		ExecutionContext temp_exec_context(context, temp_thread_context);

		idx_t payload_idx = 0;
		idx_t next_payload_idx = 0;

		//! Copy of the payload chunk, used to store the data of the radix table for use with the expression executor
		//! We can not directly use the payload chunk because the input and the output to the expression executor can
		//! not be the same Vector
		DataChunk expression_executor_input;
		expression_executor_input.InitializeEmpty(payload_chunk.GetTypes());
		expression_executor_input.SetCardinality(0);

		for (idx_t i = 0; i < aggregates.size(); i++) {
			auto &aggregate = (BoundAggregateExpression &)*aggregates[i];

			// Forward the payload idx
			payload_idx = next_payload_idx;
			next_payload_idx = payload_idx + aggregate.children.size();

			// If aggregate is not distinct, skip it
			if (!distinct_aggregate_data.IsDistinct(i)) {
				continue;
			}
			D_ASSERT(distinct_aggregate_data.table_map.count(i));
			auto table_idx = distinct_aggregate_data.table_map[i];
			auto &radix_table_p = distinct_aggregate_data.radix_tables[table_idx];
			auto &output_chunk = *distinct_aggregate_data.distinct_output_chunks[table_idx];

			//! Create global and local state for the hashtable
			auto global_source_state = radix_table_p->GetGlobalSourceState(context);
			auto local_source_state = radix_table_p->GetLocalSourceState(temp_exec_context);

			//! Retrieve the stored data from the hashtable
			while (true) {
				payload_chunk.Reset();
				output_chunk.Reset();
				radix_table_p->GetData(temp_exec_context, output_chunk,
				                       *distinct_aggregate_data.radix_states[table_idx], *global_source_state,
				                       *local_source_state);
				if (output_chunk.size() == 0) {
					break;
				}

				for (idx_t child_idx = 0; child_idx < aggregate.children.size(); child_idx++) {
					expression_executor_input.data[payload_idx + child_idx].Reference(output_chunk.data[child_idx]);
				}
				expression_executor_input.SetCardinality(output_chunk);
				// We dont need to resolve the filter, we already did this in Sink
				distinct_aggregate_data.child_executor.SetChunk(expression_executor_input);

				payload_chunk.SetCardinality(output_chunk);

#ifdef DEBUG
				gstate.state.counts[i] += payload_chunk.size();
#endif

				// resolve the child expressions of the aggregate (if any)
				idx_t payload_cnt = 0;
				for (auto &child : aggregate.children) {
					//! Before executing, remap the indices to point to the payload_chunk
					//! Originally these indices correspond to the 'input' chunk
					//! So we need to filter out the data that was used for filters (if any)
					auto &child_ref = (BoundReferenceExpression &)*child;
					child_ref.index = payload_idx + payload_cnt;

					//! The child_executor contains a pointer to the expression we altered above
					distinct_aggregate_data.child_executor.ExecuteExpression(
					    payload_idx + payload_cnt, payload_chunk.data[payload_idx + payload_cnt]);
					payload_cnt++;
				}

				auto start_of_input = payload_cnt ? &payload_chunk.data[payload_idx] : nullptr;
				//! Update the aggregate state
				AggregateInputData aggr_input_data(aggregate.bind_info.get(), Allocator::DefaultAllocator());
				aggregate.function.simple_update(start_of_input, aggr_input_data, payload_cnt,
				                                 gstate.state.aggregates[i].get(), payload_chunk.size());
			}
		}
		D_ASSERT(!gstate.finished);
		gstate.finished = true;
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		AggregateDistinct();
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

private:
	shared_ptr<Event> event;
	UngroupedAggregateGlobalState &gstate;
	ClientContext &context;
	const PhysicalUngroupedAggregate &op;
};

// TODO: Create tasks and run these in parallel instead of doing this all in Schedule, single threaded
class DistinctAggregateFinalizeEvent : public BasePipelineEvent {
public:
	DistinctAggregateFinalizeEvent(const PhysicalUngroupedAggregate &op_p, UngroupedAggregateGlobalState &gstate_p,
	                               Pipeline &pipeline_p, ClientContext &context)
	    : BasePipelineEvent(pipeline_p), op(op_p), gstate(gstate_p), context(context) {
	}
	const PhysicalUngroupedAggregate &op;
	UngroupedAggregateGlobalState &gstate;
	ClientContext &context;

public:
	void Schedule() override {
		vector<unique_ptr<Task>> tasks;
		tasks.push_back(
		    make_unique<DistinctAggregateFinalizeTask>(pipeline->executor, shared_from_this(), gstate, context, op));
		D_ASSERT(!tasks.empty());
		SetTasks(move(tasks));
	}
};

class DistinctCombineFinalizeEvent : public BasePipelineEvent {
public:
	DistinctCombineFinalizeEvent(const PhysicalUngroupedAggregate &op_p, UngroupedAggregateGlobalState &gstate_p,
	                             Pipeline &pipeline_p, ClientContext &client)
	    : BasePipelineEvent(pipeline_p), op(op_p), gstate(gstate_p), client(client) {
	}

	const PhysicalUngroupedAggregate &op;
	UngroupedAggregateGlobalState &gstate;
	ClientContext &client;

public:
	void Schedule() override {
		auto &distinct_data = *gstate.distinct_data;

		//! Now that all tables are combined, it's time to do the distinct aggregations
		auto new_event = make_shared<DistinctAggregateFinalizeEvent>(op, gstate, *pipeline, client);
		this->InsertEvent(move(new_event));

		vector<unique_ptr<Task>> tasks;
		for (idx_t table_idx = 0; table_idx < distinct_data.radix_tables.size(); table_idx++) {
			distinct_data.radix_tables[table_idx]->ScheduleTasks(pipeline->executor, shared_from_this(),
			                                                     *distinct_data.radix_states[table_idx], tasks);
		}
		D_ASSERT(!tasks.empty());
		SetTasks(move(tasks));
	}
};

SinkFinalizeType PhysicalUngroupedAggregate::FinalizeDistinct(Pipeline &pipeline, Event &event, ClientContext &context,
                                                              GlobalSinkState &gstate_p) const {
	auto &gstate = (UngroupedAggregateGlobalState &)gstate_p;
	D_ASSERT(gstate.distinct_data);
	auto &distinct_aggregate_data = *gstate.distinct_data;
	auto &payload_chunk = distinct_aggregate_data.payload_chunk;

	//! Copy of the payload chunk, used to store the data of the radix table for use with the expression executor
	//! We can not directly use the payload chunk because the input and the output to the expression executor can not be
	//! the same Vector
	DataChunk expression_executor_input;
	expression_executor_input.InitializeEmpty(payload_chunk.GetTypes());
	expression_executor_input.SetCardinality(0);

	bool any_partitioned = false;
	for (idx_t table_idx = 0; table_idx < distinct_aggregate_data.radix_tables.size(); table_idx++) {
		auto &radix_table_p = distinct_aggregate_data.radix_tables[table_idx];
		auto &radix_state = *distinct_aggregate_data.radix_states[table_idx];
		bool partitioned = radix_table_p->Finalize(context, radix_state);
		if (partitioned) {
			any_partitioned = true;
		}
	}
	if (any_partitioned) {
		auto new_event = make_shared<DistinctCombineFinalizeEvent>(*this, gstate, pipeline, context);
		event.InsertEvent(move(new_event));
	} else {
		//! Hashtables aren't partitioned, they dont need to be joined first
		//! So we can compute the aggregate already
		auto new_event = make_shared<DistinctAggregateFinalizeEvent>(*this, gstate, pipeline, context);
		event.InsertEvent(move(new_event));
	}
	return SinkFinalizeType::READY;
}

SinkFinalizeType PhysicalUngroupedAggregate::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                      GlobalSinkState &gstate_p) const {
	auto &gstate = (UngroupedAggregateGlobalState &)gstate_p;

	if (gstate.distinct_data) {
		return FinalizeDistinct(pipeline, event, context, gstate_p);
	}

	D_ASSERT(!gstate.finished);
	gstate.finished = true;
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class UngroupedAggregateState : public GlobalSourceState {
public:
	UngroupedAggregateState() : finished(false) {
	}

	bool finished;
};

unique_ptr<GlobalSourceState> PhysicalUngroupedAggregate::GetGlobalSourceState(ClientContext &context) const {
	return make_unique<UngroupedAggregateState>();
}

void VerifyNullHandling(DataChunk &chunk, AggregateState &state, const vector<unique_ptr<Expression>> &aggregates) {
#ifdef DEBUG
	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		auto &aggr = (BoundAggregateExpression &)*aggregates[aggr_idx];
		if (state.counts[aggr_idx] == 0 && aggr.function.null_handling == FunctionNullHandling::DEFAULT_NULL_HANDLING) {
			// Default is when 0 values go in, NULL comes out
			UnifiedVectorFormat vdata;
			chunk.data[aggr_idx].ToUnifiedFormat(1, vdata);
			D_ASSERT(!vdata.validity.RowIsValid(vdata.sel->get_index(0)));
		}
	}
#endif
}

void PhysicalUngroupedAggregate::GetData(ExecutionContext &context, DataChunk &chunk, GlobalSourceState &gstate_p,
                                         LocalSourceState &lstate) const {
	auto &gstate = (UngroupedAggregateGlobalState &)*sink_state;
	auto &state = (UngroupedAggregateState &)gstate_p;
	D_ASSERT(gstate.finished);
	if (state.finished) {
		return;
	}

	// initialize the result chunk with the aggregate values
	chunk.SetCardinality(1);
	for (idx_t aggr_idx = 0; aggr_idx < aggregates.size(); aggr_idx++) {
		auto &aggregate = (BoundAggregateExpression &)*aggregates[aggr_idx];

		Vector state_vector(Value::POINTER((uintptr_t)gstate.state.aggregates[aggr_idx].get()));
		AggregateInputData aggr_input_data(aggregate.bind_info.get(), Allocator::DefaultAllocator());
		aggregate.function.finalize(state_vector, aggr_input_data, chunk.data[aggr_idx], 1, 0);
	}
	VerifyNullHandling(chunk, gstate.state, aggregates);
	state.finished = true;
}

string PhysicalUngroupedAggregate::ParamsToString() const {
	string result;
	for (idx_t i = 0; i < aggregates.size(); i++) {
		auto &aggregate = (BoundAggregateExpression &)*aggregates[i];
		if (i > 0) {
			result += "\n";
		}
		result += aggregates[i]->GetName();
		if (aggregate.filter) {
			result += " Filter: " + aggregate.filter->GetName();
		}
	}
	return result;
}

} // namespace duckdb
