daukle.plugin{ api = 1, uses = { "env", "cache" } }

daukle.resolver{
  resolve = function(coordinate)
    -- Cached by the coordinate (the cache's "project" slot, the one that may
    -- hold a single slash), so an ordinary second run reuses this answer and
    -- only "daukle plugin update", which turns the cache off, sees a new target.
    local url = daukle.cache(coordinate, "resolver-from-env", "resolve", function()
      return "https://example.invalid/" .. daukle.env("RESOLVER_TARGET") .. ".lua"
    end)
    return { url = url, resolved = coordinate }
  end,
}
