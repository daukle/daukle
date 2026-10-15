daukle.plugin{ api = 2, uses = { "region" } }

daukle.language{
  name = "overridden-deps",
  apply = function(consumer, resolved, text) return text end,
}
