namespace VulkanLearn;

// 规则按优先级降序、同优先级按注册顺序；一次 Update 最多切换一次，不隐式连跳。
public sealed class StateMachine<TState>(TState initial) where TState : struct, Enum
{
    private sealed record Transition(TState From, TState To, int Priority, Func<bool> Condition);
    private readonly List<Transition> transitions = [];
    public TState Current { get; private set; } = initial;
    public void Add(TState from, TState to, Func<bool> condition, int priority = 0)
    {
        ArgumentNullException.ThrowIfNull(condition);
        var transition = new Transition(from, to, priority, condition);
        int index = transitions.FindIndex(t => t.Priority < priority);
        if (index < 0) transitions.Add(transition); else transitions.Insert(index, transition);
    }
    public bool Update()
    {
        foreach (var transition in transitions)
            if (EqualityComparer<TState>.Default.Equals(Current, transition.From) && transition.Condition())
            {
                Current = transition.To;
                return true;
            }
        return false;
    }
}
