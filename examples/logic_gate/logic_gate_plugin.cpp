#include <speculor/plugin_helpers.h>

#include <cstring>

// --- state ---

struct LogicGateState
{
    spc::HostServices host;

    // params
    int32_t function = 0;       // 0=AND, 1=OR, 2=XOR, 3=NAND, 4=NOR, 5=MAJORITY, 6=THRESHOLD
    int32_t input_count = 2;    // 2-4 active inputs
    int32_t threshold = 2;      // for THRESHOLD mode: at least N inputs true
    int32_t invert_0 = 0;
    int32_t invert_1 = 0;
    int32_t invert_2 = 0;
    int32_t invert_3 = 0;
    int32_t edge_mode = 0;      // 0=level, 1=rising, 2=falling, 3=both

    // control message output
    char cmd_param[SPC_PARAM_STRING_MAX] = "";
    float cmd_true_value = 1.0f;
    float cmd_false_value = 0.0f;

    // runtime state
    bool last_values[4] = {};   // last-seen per-input values (for stale non-blocking inputs)
    bool prev_result = false;
    bool has_prev = false;
};

SPC_PLUGIN(LogicGateState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("logic_gate", "Logic Gate", "Automation/Logic")
        .author("Speculor").version("0.1.0")
        .description("Boolean logic operations on up to 4 scalar inputs. Combines alert "
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
        .output("cmd_out", "Param Command", SPC_DATA_CONTROL_MSG)
        // logic function
        .enum_param("function", "Function",
                    {"AND", "OR", "XOR", "NAND", "NOR", "MAJORITY", "THRESHOLD"},
                    0, "Logic")
        .param_description("Logic operation to apply to the active inputs")
        .int_param("input_count", "Active Inputs", 2, 4, 2, 1, "Logic")
        .param_description("Number of input ports to evaluate (unused inputs read as false)")
        .int_param("threshold", "Threshold Count", 1, 4, 2, 1, "Logic")
        .param_description("Minimum number of true inputs required (THRESHOLD mode only)")
        // per-input invert
        .bool_param("invert_0", "Invert Input 0", false, "Invert")
        .bool_param("invert_1", "Invert Input 1", false, "Invert")
        .bool_param("invert_2", "Invert Input 2", false, "Invert")
        .bool_param("invert_3", "Invert Input 3", false, "Invert")
        // edge detection
        .enum_param("edge_mode", "Edge Mode",
                    {"Level (continuous)", "Rising Edge", "Falling Edge", "Both Edges"},
                    0, "Output")
        .param_description("When to emit the control message: continuously, or only on transitions")
        // control message
        .string_param("cmd_param", "Target Param", "", "Param Command")
        .param_description("Parameter name to set on downstream node (empty = disabled)")
        .float_param("cmd_true_value", "True Value", -1e6f, 1e6f, 1.0f, 0.1f, "Param Command")
        .param_description("Value sent when result becomes true")
        .float_param("cmd_false_value", "False Value", -1e6f, 1e6f, 0.0f, 0.1f, "Param Command")
        .param_description("Value sent when result becomes false")
        .streaming()
)

SPC_PLUGIN_AUTO_PARAMS(LogicGateState,
    SPC_BIND_ENUM(LogicGateState, "function", function),
    SPC_BIND_INT(LogicGateState, "input_count", input_count),
    SPC_BIND_INT(LogicGateState, "threshold", threshold),
    SPC_BIND_BOOL(LogicGateState, "invert_0", invert_0),
    SPC_BIND_BOOL(LogicGateState, "invert_1", invert_1),
    SPC_BIND_BOOL(LogicGateState, "invert_2", invert_2),
    SPC_BIND_BOOL(LogicGateState, "invert_3", invert_3),
    SPC_BIND_ENUM(LogicGateState, "edge_mode", edge_mode),
    SPC_BIND_STRING(LogicGateState, "cmd_param", cmd_param),
    SPC_BIND_FLOAT(LogicGateState, "cmd_true_value", cmd_true_value),
    SPC_BIND_FLOAT(LogicGateState, "cmd_false_value", cmd_false_value)
)

// --- lifecycle ---

static int start(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    for (auto& v : s->last_values) v = false;
    s->prev_result = false;
    s->has_prev = false;
    SPC_LOG_INFO(&s->host.cached_log, "Logic Gate started (function=%d, inputs=%d)",
                 s->function, s->input_count);
    return 0;
}

static int stop(SpcPluginInstance* inst)
{
    auto* s = state(inst);
    SPC_LOG_INFO(&s->host.cached_log, "Logic Gate stopped");
    return 0;
}

// --- evaluate ---

static bool evaluate(LogicGateState* s, const bool* inputs, int n)
{
    int true_count = 0;
    for (int i = 0; i < n; ++i)
        if (inputs[i]) ++true_count;

    switch (s->function) {
        case 0: return true_count == n;                             // AND
        case 1: return true_count > 0;                              // OR
        case 2: return (true_count % 2) == 1;                       // XOR
        case 3: return true_count < n;                               // NAND
        case 4: return true_count == 0;                              // NOR
        case 5: return true_count > n / 2;                           // MAJORITY (strict)
        case 6: return true_count >= s->threshold;                   // THRESHOLD
        default: return false;
    }
}

// --- process ---

static int process(SpcPluginInstance* inst, const SpcData* inputs, uint32_t input_count,
                   SpcData* outputs, uint32_t output_count)
{
    auto* s = state(inst);

    // read inputs — non-blocking, so missing inputs keep their last value
    const int32_t invert[4] = {s->invert_0, s->invert_1, s->invert_2, s->invert_3};
    const int n = std::min(s->input_count, static_cast<int32_t>(input_count));

    for (int i = 0; i < n; ++i) {
        if (inputs[i].type == SPC_DATA_SCALAR) {
            s->last_values[i] = spc::scalar_as_bool(inputs[i].scalar);
        }
        // else: keep last_values[i] from previous cycle
    }
    // inactive inputs are false
    for (int i = n; i < 4; ++i) {
        s->last_values[i] = false;
    }

    bool vals[4];
    for (int i = 0; i < 4; ++i)
        vals[i] = invert[i] ? !s->last_values[i] : s->last_values[i];

    bool result = evaluate(s, vals, s->input_count);

    // edge detection
    bool rising = result && (!s->prev_result || !s->has_prev);
    bool falling = !result && s->prev_result && s->has_prev;

    if (output_count >= 1) {
        outputs[0].type = SPC_DATA_SCALAR;
        outputs[0].scalar.type = SPC_FIELD_INT32;
        outputs[0].scalar.b = result ? 1 : 0;
    }

    // emit control message based on edge mode
    if (output_count >= 2 && s->cmd_param[0] != '\0') {
        bool should_emit = false;
        float cmd_value = 0.0f;

        switch (s->edge_mode) {
            case 0: // level — always emit current state
                should_emit = true;
                cmd_value = result ? s->cmd_true_value : s->cmd_false_value;
                break;
            case 1: // rising edge
                if (rising) { should_emit = true; cmd_value = s->cmd_true_value; }
                break;
            case 2: // falling edge
                if (falling) { should_emit = true; cmd_value = s->cmd_false_value; }
                break;
            case 3: // both edges
                if (rising) { should_emit = true; cmd_value = s->cmd_true_value; }
                else if (falling) { should_emit = true; cmd_value = s->cmd_false_value; }
                break;
        }

        if (should_emit) {
            SpcControlMsg cmd{};
            spc::param_cmd_clear(&cmd);
            spc::param_cmd_set_float(&cmd, s->cmd_param, cmd_value);
            outputs[1].type = SPC_DATA_CONTROL_MSG;
            outputs[1].control_msg = &cmd;
        }
    }

    s->prev_result = result;
    s->has_prev = true;

    return 0;
}

SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .start             = start,
    .stop              = stop,
    .set_host_services = set_host_services
)
