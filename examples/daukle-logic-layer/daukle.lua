local FEATURES = { "logging", "metrics" }

local mode = daukle.env("DAUKLE_EXAMPLE_DEV") == "1" and "debug" or "release"

daukle.config.toolchains = {
  banner = { mode = mode, features = FEATURES },
}

daukle.log("generating a " .. mode .. " banner with " .. #FEATURES .. " features for "
           .. daukle.host.os .. "/" .. daukle.host.arch)
