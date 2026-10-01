daukle.plugin{ api = 1, uses = { "tool", "exec", "env" } }

local function publishes_marker(marker)
  return function(context)
    local child = daukle.tool(daukle.env("DAUKLE_TEST_CHILD"))
    daukle.exec(child, { "--task-child", marker })
  end
end

daukle.publisher{ name = "alpha", publish = publishes_marker("alpha-ran.txt") }
daukle.publisher{ name = "beta", publish = publishes_marker("beta-ran.txt") }
