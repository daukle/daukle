daukle.plugin{ api = 10, uses = { "write", "read" } }

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.task{
  name = "stub:build",
  run = function(context)
    daukle.write{ path = "greeting.txt", text = "hello" }
  end,
}

local function within(dir, path)
  if dir == nil then return path end
  return dir .. "/" .. path
end

--- The directories to hand back, read from `instances.txt` beside the detection: one per line.
local function handed(context)
  local listed = daukle.read(within(context.dir, "instances.txt"))
  local out = {}
  for line in string.gmatch(listed or "", "[^\n]+") do out[#out + 1] = line end
  return out
end

daukle.migrator{
  tool = "faketool",
  toolchain = "stub",
  run = function(context)
    return {
      manifest = { toolchains = { [context.instance] = {} } },
      foreign = {},
      blockers = {},
      owned = { within(context.dir, "faketool.conf"), within(context.dir, "out") },
      instances = handed(context),
      proof = {
        task = context.instance .. ":build",
        compare = { { old = within(context.dir, "out/greeting.txt"),
                      new = "build/daukle/" .. context.instance .. "/greeting.txt", as = "bytes" } },
      },
    }
  end,
}
