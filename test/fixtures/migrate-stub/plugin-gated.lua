daukle.plugin{ api = 3, uses = { "write" } }

if daukle.manifest ~= "" and string.match(daukle.manifest, "%.toml$") == nil then error("unexpected manifest name") end

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.task{
  name = "stub:build",
  run = function()
    daukle.write{ path = "greeting.txt", text = "hello" }
  end,
}

daukle.migrator{
  tool = "faketool",
  toolchain = "stub",
  run = function(context)
    return {
      manifest = { toolchains = { [context.instance] = {} } },
      foreign = {},
      blockers = {},
      owned = { "faketool.conf", "out" },
      proof = {
        task = context.instance .. ":build",
        compare = { { old = "out/greeting.txt", new = "build/daukle/" .. context.instance .. "/greeting.txt", as = "bytes" } },
      },
    }
  end,
}
