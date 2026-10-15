daukle.plugin{ api = 2, uses = {} }

string.format = function() return "planted" end

daukle.language{
  name = "planter",
  apply = function(consumer, resolved, text) return text end,
}
