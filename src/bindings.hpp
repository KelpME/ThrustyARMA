#pragma once

#include <linux/input-event-codes.h>
#include <vector>
#include <map>
#include <cstdint>
#include <optional>

// Forward declarations to avoid circular includes
struct BindingConfigKey;
struct BindingConfigAbs;

enum class Role {
    Stick,
    Throttle,
    Rudder
};

enum class SrcKind {
    Key,
    Abs
};

struct PhysicalInput {
    Role role;
    SrcKind kind;
    uint16_t code;
    
    bool operator<(const PhysicalInput& other) const {
        if (role != other.role) return role < other.role;
        if (kind != other.kind) return kind < other.kind;
        return code < other.code;
    }
};

struct VirtualSlot {
    SrcKind kind;
    uint16_t code;
    
    bool operator<(const VirtualSlot& other) const {
        if (kind != other.kind) return kind < other.kind;
        return code < other.code;
    }
    
    bool operator==(const VirtualSlot& other) const {
        return kind == other.kind && code == other.code;
    }
};

struct AxisTransform {
    bool invert = false;
    int deadzone = 0;
    float scale = 1.0f;
    int min_out;
    int max_out;
};

struct Binding {
    PhysicalInput src;
    VirtualSlot dst;
    AxisTransform xform;
};

class BindingResolver {
private:
    std::vector<Binding> bindings;
    std::map<VirtualSlot, int> button_refcounts;
    std::map<VirtualSlot, std::map<PhysicalInput, bool>> button_pressed_sources;
    std::map<VirtualSlot, std::map<Role, std::optional<int>>> axis_values;
    std::map<VirtualSlot, std::optional<Role>> axis_selected_source;
    std::map<VirtualSlot, int> last_output_values;
    std::map<Role, int> bit_depth_overrides;  // Explicit bit depths per role (0 = auto)
    
    static Role get_role_priority(const VirtualSlot& dst);
    bool is_virtual_slot_valid(const VirtualSlot& slot) const;
    
public:
    BindingResolver(const std::vector<Binding>& bindings);
    void set_bit_depth(Role role, int bit_depth);
    
    void process_input(const PhysicalInput& input, int value);
    std::vector<std::pair<VirtualSlot, int>> get_pending_events();
    void clear_pending_events();
    
    // Public for diagnostics
    int apply_axis_transform(int value, const AxisTransform& xform, Role role) const;
};

std::vector<Binding> make_default_bindings();
std::vector<Binding> make_bindings_from_config(const std::vector<BindingConfigKey>& config_keys, const std::vector<BindingConfigAbs>& config_abs);

bool validate_bindings(const std::vector<Binding>& bindings);

#ifdef DEBUG_BINDINGS
extern bool debug_bindings_enabled;
#define DEBUG_LOG(...) if (debug_bindings_enabled) { printf(__VA_ARGS__); }
#else
#define DEBUG_LOG(...)
#endif