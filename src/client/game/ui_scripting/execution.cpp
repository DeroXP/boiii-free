#include <std_include.hpp>

#include "execution.hpp"
#include "stack_guard.hpp"

namespace ui_scripting {
using namespace game::lua::hks;
namespace {
// The message of the error a failed protected call left on the stack
std::string error_message(lua_State *state) {
  const HksObject *top = state->m_apistack.top - 1;
  if (top >= state->m_apistack.base && top->t == HksObjectType::TSTRING &&
      top->v.str) {
    return top->v.str->m_data;
  }
  return "a Lua error";
}

// Xerces: object[key] inside a protected call (see call_script_function):
// a metamethod's error on a read from C++ outside any Lua call crashed the
// game once (gpu::on_main_menu, docs/TESTING.md). Arguments: object, key.
luaReturnCount_e protected_gettable(lua_State *s) {
  HksObject value{};
  hks_obj_gettable(&value, s, &s->m_apistack.base[0], &s->m_apistack.base[1]);
  *s->m_apistack.top = value;
  s->m_apistack.top++;
  return static_cast<luaReturnCount_e>(1);
}

script_value get_field(void *ptr, HksObjectType type, const script_value &key) {
  lua_State *state = *primary_luaVM;
  const detail::api_stack_guard stack_guard{state};

  HksObject value{};
  HksObject obj{};
  obj.t = type;
  obj.v.ptr = ptr;

  if (hksi_lua_pcall.get()) {
    HksObject getter{};
    getter.t = HksObjectType::TCFUNCTION;
    getter.v.cClosure = cclosure_Create(state, protected_gettable, 0, 0, 0);
    push_value(getter);
    push_value(obj);
    push_value(key);
    if (hksi_lua_pcall(state, 2, 1, 0) != 0) {
      throw std::runtime_error(error_message(state));
    }
    return state->m_apistack.top[-1];
  }

  push_value(key);
  hks_obj_gettable(&value, state, &obj, &state->m_apistack.top[-1]);
  return value;
}

// object[key] = value inside a protected call, as protected_gettable.
// Arguments: object, key, value.
luaReturnCount_e protected_settable(lua_State *s) {
  hks_obj_settable(s, &s->m_apistack.base[0], &s->m_apistack.base[1],
                   &s->m_apistack.base[2]);
  return static_cast<luaReturnCount_e>(0);
}

void set_field(void *ptr, HksObjectType type, const script_value &key,
               const script_value &value) {
  lua_State *state = *primary_luaVM;

  HksObject obj{};
  obj.t = type;
  obj.v.ptr = ptr;

  if (hksi_lua_pcall.get()) {
    const detail::api_stack_guard stack_guard{state};
    HksObject setter{};
    setter.t = HksObjectType::TCFUNCTION;
    setter.v.cClosure = cclosure_Create(state, protected_settable, 0, 0, 0);
    push_value(setter);
    push_value(obj);
    push_value(key);
    push_value(value);
    if (hksi_lua_pcall(state, 3, 0, 0) != 0) {
      throw std::runtime_error(error_message(state));
    }
    return;
  }

  hks_obj_settable(state, &obj, &key.get_raw(), &value.get_raw());
}
} // namespace

void push_value(const script_value &value) {
  lua_State *state = *primary_luaVM;
  *state->m_apistack.top = value.get_raw();
  state->m_apistack.top++;
}

void push_value(const HksObject &value) {
  lua_State *state = *primary_luaVM;
  *state->m_apistack.top = value;
  state->m_apistack.top++;
}

script_value get_return_value(std::int64_t offset) {
  lua_State *state = *primary_luaVM;
  return state->m_apistack.top[-1 - offset];
}

arguments get_return_values() {
  lua_State *state = *primary_luaVM;
  const auto count = state->m_apistack.top - state->m_apistack.base;
  arguments values;

  for (auto i = count - 1; i >= 0; i--) {
    values.push_back(get_return_value(i));
  }

  if (values.empty()) {
    values.push_back({});
  }

  return values;
}

arguments get_return_values(HksObject *base) {
  lua_State *state = *primary_luaVM;
  const auto count = state->m_apistack.top - base;
  arguments values;

  for (auto i = count - 1; i >= 0; i--) {
    values.push_back(get_return_value(i));
  }

  if (values.empty()) {
    values.push_back({});
  }

  return values;
}

bool notify(const std::string &name, const event_arguments &arguments) {
  lua_State *state = *primary_luaVM;
  if (state == nullptr) {
    return false;
  }

  // const auto _0 = gsl::finally(game::LUI_LeaveCriticalSection);
  // game::LUI_EnterCriticalSection();

  try {
    const auto globals = table((*primary_luaVM)->globals.v.table);
    const auto engine = globals.get("Engine").as<table>();
    const auto root = engine.get("GetLuiRoot")()[0].as<userdata>();
    const auto process_event = root.get("processEvent");

    table event{};
    event.set("name", name);
    event.set("dispatchChildren", true);

    for (const auto &arg : arguments) {
      event.set(arg.first, arg.second);
    }

    process_event(root, event);
    return true;
  } catch (const std::exception &ex) {
    printf("Error processing event '%s' %s\n", name.data(), ex.what());
  }

  return false;
}

arguments call_script_function(const function &function,
                               const arguments &arguments) {
  lua_State *state = *primary_luaVM;
  const detail::api_stack_guard stack_guard{state};

  push_value(function);
  for (auto i = arguments.begin(); i != arguments.end(); ++i) {
    push_value(*i);
  }

  // Xerces: a protected call. Unprotected, a Lua error with no protected call
  // to unwind to hits the engine's int3 (0x141D3C84B, under hksi_lua_error):
  // a crash whenever C++ calls into Lua from outside a Lua call, as from a
  // scheduler callback (xerces_crash_test luaerror, docs/TESTING.md). Here it
  // becomes a C++ exception, which the callers catch; inside a C function Lua
  // called, main_handler turns it back into a Lua error.
  if (hksi_lua_pcall.get()) {
    const int32_t status =
        hksi_lua_pcall(state, static_cast<int32_t>(arguments.size()), -1, 0);
    if (status != 0) {
      throw std::runtime_error(error_message(state));
    }
    return get_return_values(stack_guard.saved_top());
  }

  hksi_lua_call(state, static_cast<int>(arguments.size()), -1, nullptr);
  return get_return_values(stack_guard.saved_top());
}

script_value get_field(const userdata &self, const script_value &key) {
  return get_field(self.ptr, HksObjectType::TUSERDATA, key);
}

script_value get_field(const table &self, const script_value &key) {
  return get_field(self.ptr, HksObjectType::TTABLE, key);
}

void set_field(const userdata &self, const script_value &key,
               const script_value &value) {
  set_field(self.ptr, HksObjectType::TUSERDATA, key, value);
}

void set_field(const table &self, const script_value &key,
               const script_value &value) {
  set_field(self.ptr, HksObjectType::TTABLE, key, value);
}
} // namespace ui_scripting
