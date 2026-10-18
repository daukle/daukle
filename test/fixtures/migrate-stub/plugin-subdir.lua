daukle.plugin{ api = 3, uses = { "write" } }

daukle.toolchain{ name = "stub", generate = function() return {} end }

daukle.task{
  name = "stub:build",
  run = function(context)
    daukle.write{ path = "greeting.txt", text = "hello" }
    daukle.write{ path = "root.txt", text = context.root }
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
      owned = { context.dir .. "/faketool.conf", context.dir .. "/out" },
      proof = {
        task = context.instance .. ":build",
        compare = { { old = context.dir .. "/out/greeting.txt", new = "build/daukle/" .. context.instance .. "/greeting.txt", as = "bytes" } },
      },
    }
  end,
}
