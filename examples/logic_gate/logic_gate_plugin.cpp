// Boolean logic over up to four scalar inputs, with a CONTROL output that sets
// a parameter on the nodes wired to it when the result changes. The commands
// go through the host, which delivers them once process() returns.

#include <speculor/plugin.h>
#include <speculor/plugin_helpers.h>

#include <new>

// The parameters, in declaration order: values[P_...] is that parameter's value.
enum Param : uint32_t {
    P_FUNCTION,
    P_INPUT_COUNT,
    P_THRESHOLD,
    P_INVERT_0,
    P_INVERT_1,
    P_INVERT_2,
    P_INVERT_3,
    P_EDGE_MODE,
    P_CMD_PARAM,
    P_CMD_TRUE_VALUE,
    P_CMD_FALSE_VALUE,
};

// the output ports, in declaration order
enum Output : uint32_t {
    OUT_RESULT,
    OUT_CMD,
};

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("logic_gate", "Logic Gate", "Automation/Logic")
        .author("Speculor").version("0.1.0")
        .help("Boolean logic operations on up to 4 scalar inputs. Combines alert "
              "outputs using AND, OR, XOR, NAND, NOR, MAJORITY, or THRESHOLD. "
              "Emits a control message on rising/falling edges to automate "
              "parameters on other nodes.")
        .maturity(SPC_MATURITY_PREVIEW)
        .tags({"common"})
        // inputs — all non-blocking so unused inputs don't stall
        .input("in_0", "Input 0", SPC_DATA_SCALAR, 4, SPC_CONSUME_NON_BLOCKING)
        .input("in_1", "Input 1", SPC_DATA_SCALAR, 4, SPC_CONSUME_NON_BLOCKING)
        .input("in_2", "Input 2", SPC_DATA_SCALAR, 4, SPC_CONSUME_NON_BLOCKING)
        .input("in_3", "Input 3", SPC_DATA_SCALAR, 4, SPC_CONSUME_NON_BLOCKING)
        // outputs
        .output("result", "Result", SPC_DATA_SCALAR)
        .output("cmd_out", "Param Command", SPC_DATA_CONTROL)
        // logic function
        .enum_param("function", "Function",
                    {"AND", "OR", "XOR", "NAND", "NOR", "MAJORITY", "THRESHOLD"},
                    0, "Logic")
        .param_help("Logic operation to apply to the active inputs")
        .int_param("input_count", "Active Inputs", 2, 4, 2, 1, "Logic")
        .param_help("Number of input ports to evaluate (unused inputs read as false)")
        .int_param("threshold", "Threshold Count", 1, 4, 2, 1, "Logic")
        .param_help("Minimum number of true inputs required (THRESHOLD mode only)")
        // per-input invert
        .bool_param("invert_0", "Invert Input 0", false, "Invert")
        .bool_param("invert_1", "Invert Input 1", false, "Invert")
        .bool_param("invert_2", "Invert Input 2", false, "Invert")
        .bool_param("invert_3", "Invert Input 3", false, "Invert")
        // edge detection
        .enum_param("edge_mode", "Edge Mode",
                    {"Level (continuous)", "Rising Edge", "Falling Edge", "Both Edges"},
                    0, "Output")
        .param_help("When to emit the control message: continuously, or only on transitions")
        // control message
        .string_param("cmd_param", "Target Param", "", "Param Command")
        .param_help("Parameter name to set on downstream node (empty = disabled)")
        .float_param("cmd_true_value", "True Value", -1e6f, 1e6f, 1.0f, 0.1f, "Param Command")
        .param_help("Value sent when result becomes true")
        .float_param("cmd_false_value", "False Value", -1e6f, 1e6f, 0.0f, 0.1f, "Param Command")
        .param_help("Value sent when result becomes false")
)

struct Params
{
    int32_t function;       // 0=AND, 1=OR, 2=XOR, 3=NAND, 4=NOR, 5=MAJORITY, 6=THRESHOLD
    int32_t input_count;    // 2-4 active inputs
    int32_t threshold;      // for THRESHOLD mode: at least N inputs true
    int32_t invert[4];
    int32_t edge_mode;      // 0=level, 1=rising, 2=falling, 3=both
    char cmd_param[SPC_PARAM_STRING_MAX];
    float cmd_true_value;
    float cmd_false_value;
};

static Params read_params(const SpcValue* values)
{
    Params p;
    p.function    = values[P_FUNCTION].choice.index;   // ENUM: the position of the option
    p.input_count = values[P_INPUT_COUNT].i;
    p.threshold   = values[P_THRESHOLD].i;
    for (uint32_t i = 0; i < 4; ++i)
        p.invert[i] = values[P_INVERT_0 + i].i;          // BOOL: 0 or 1 in .i
    p.edge_mode   = values[P_EDGE_MODE].choice.index;
    // a STRING value is the host's and lives only during the call: copy it
    spc::copy_str(p.cmd_param, sizeof(p.cmd_param), values[P_CMD_PARAM].str.ptr);
    p.cmd_true_value  = values[P_CMD_TRUE_VALUE].f;
    p.cmd_false_value = values[P_CMD_FALSE_VALUE].f;
    return p;
}

struct LogicGateState
{
    SpcHost* host = nullptr;    // valid until stop() returns
    SpcLogContext log{};
    Params params{};

    bool last_values[4] = {};   // last-seen per-input values (for stale non-blocking inputs)
    bool prev_result = false;
    bool has_prev = false;
};

static LogicGateState* state(SpcPluginInstance* instance)
{
    return reinterpret_cast<LogicGateState*>(instance);
}

static SpcPluginInstance* start(SpcHost* host, const SpcValue* values)
{
    auto* s = new (std::nothrow) LogicGateState{};
    if (!s) return nullptr;
    s->host = host;
    s->log = spc::log_context(host);   // a convenience over host->log, for the SPC_LOG_* macros
    s->params = read_params(values);
    SPC_LOG_INFO(&s->log, "Logic Gate started (function=%d, inputs=%d)",
                 s->params.function, s->params.input_count);
    return reinterpret_cast<SpcPluginInstance*>(s);
}

static void stop(SpcPluginInstance* instance)
{
    auto* s = state(instance);
    SPC_LOG_INFO(&s->log, "Logic Gate stopped");
    delete s;
}

static void params_changed(SpcPluginInstance* instance, const SpcValue* values,
                           uint64_t /*changed*/)
{
    state(instance)->params = read_params(values);
}

static bool evaluate(const Params& p, const bool* inputs, int n)
{
    int true_count = 0;
    for (int i = 0; i < n; ++i)
        if (inputs[i]) ++true_count;

    switch (p.function) {
        case 0: return true_count == n;                             // AND
        case 1: return true_count > 0;                              // OR
        case 2: return (true_count % 2) == 1;                       // XOR
        case 3: return true_count < n;                              // NAND
        case 4: return true_count == 0;                             // NOR
        case 5: return true_count > n / 2;                          // MAJORITY (strict)
        case 6: return true_count >= p.threshold;                   // THRESHOLD
        default: return false;
    }
}

static int process(SpcPluginInstance* instance, SpcStep* step)
{
    auto* s = state(instance);
    const Params& p = s->params;
    const SpcData* inputs = step->inputs;

    // Non-blocking inputs: a port with nothing new this step arrives as
    // SPC_DATA_NONE, and keeps its last value. spc::scalar_as_bool is a
    // convenience: any scalar type, as a bool.
    const int n = p.input_count;
    for (int i = 0; i < n; ++i) {
        if (inputs[i].type == SPC_DATA_SCALAR)
            s->last_values[i] = spc::scalar_as_bool(inputs[i].scalar);
    }
    // inactive inputs are false
    for (int i = n; i < 4; ++i)
        s->last_values[i] = false;

    bool vals[4];
    for (int i = 0; i < 4; ++i)
        vals[i] = p.invert[i] ? !s->last_values[i] : s->last_values[i];

    const bool result = evaluate(p, vals, n);

    // edge detection
    const bool rising = result && (!s->prev_result || !s->has_prev);
    const bool falling = !result && s->prev_result && s->has_prev;

    SpcData& out = step->outputs[OUT_RESULT];
    out.type = SPC_DATA_SCALAR;
    out.scalar = {};   // timestamp_ns and sequence 0: the engine stamps them
    out.scalar.type = SPC_FIELD_INT32;
    out.scalar.b = result ? 1 : 0;

    // emit a control command based on edge mode
    if (p.cmd_param[0] != '\0') {
        bool should_emit = false;
        float cmd_value = 0.0f;

        switch (p.edge_mode) {
            case 0: // level — always emit current state
                should_emit = true;
                cmd_value = result ? p.cmd_true_value : p.cmd_false_value;
                break;
            case 1: // rising edge
                if (rising) { should_emit = true; cmd_value = p.cmd_true_value; }
                break;
            case 2: // falling edge
                if (falling) { should_emit = true; cmd_value = p.cmd_false_value; }
                break;
            case 3: // both edges
                if (rising) { should_emit = true; cmd_value = p.cmd_true_value; }
                else if (falling) { should_emit = true; cmd_value = p.cmd_false_value; }
                break;
        }

        // spc::ControlOut is a convenience over host->control(host, OUT_CMD,
        // SPC_CONTROL_SET, name, SPC_PARAM_FLOAT, &value, 0): the value goes in
        // an SpcValue, and request id 0 asks for no reply. The host sends the
        // command to every node wired to the output once process() returns.
        if (should_emit)
            spc::ControlOut(s->host, OUT_CMD, 0).set_float(p.cmd_param, cmd_value);
    }

    s->prev_result = result;
    s->has_prev = true;

    return SPC_OK;
}

SPC_EXPORT_PLUGIN(
    .info           = info,
    .start          = start,
    .stop           = stop,
    .process        = process,
    .params_changed = params_changed
)
