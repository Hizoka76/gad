#pragma once
// Constantes Qt issues du même manifeste que les types Rust générés par build.rs.
#include <QtCore>
namespace Protocol {
namespace State {
#define GAD_PROTOCOL_VERSION(number)
#define GAD_COMMAND(name, wire)
#define GAD_STATE(name, wire, terminal) inline constexpr auto name = #wire;
#include "protocol.def"
#undef GAD_STATE
#undef GAD_COMMAND
#undef GAD_PROTOCOL_VERSION
}
namespace Command {
#define GAD_PROTOCOL_VERSION(number) inline constexpr int Version = number;
#define GAD_STATE(name, wire, terminal)
#define GAD_COMMAND(name, wire) inline constexpr auto name = #wire;
#include "protocol.def"
#undef GAD_STATE
#undef GAD_COMMAND
#undef GAD_PROTOCOL_VERSION
}
/// Le même indicateur de terminaison sert aux notifications, à la fermeture et à l’affichage.
inline bool terminal(const QString &value) {
#define GAD_PROTOCOL_VERSION(number)
#define GAD_COMMAND(name, wire)
#define GAD_STATE(name, wire, stopped) if(value == State::name)return stopped;
#include "protocol.def"
#undef GAD_STATE
#undef GAD_COMMAND
#undef GAD_PROTOCOL_VERSION
    return false;
}
}
