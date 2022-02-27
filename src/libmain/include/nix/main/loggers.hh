#pragma once
///@file

#include "nix/util/types.hh"

namespace nix {

enum class LogFormat {
  raw,
  rawWithLogs,
  internalJSON,
  diffs,
  bar,
  barWithLogs,
};

void setLogFormat(const std::string & logFormatStr);
void setLogFormat(const LogFormat & logFormat);

/**
 * Turn on build logs for the current logger and for any logger created afterwards, so that
 * `--print-build-logs` works whichever side of `--log-format` it appears on.
 */
void setDefaultPrintBuildLogs(bool printBuildLogs);

} // namespace nix
