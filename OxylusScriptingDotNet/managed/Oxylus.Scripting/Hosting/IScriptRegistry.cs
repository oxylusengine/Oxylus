namespace Oxylus.Hosting;

// implemented by the generated Oxylus.Generated.ScriptRegistry in every scripts assembly, called in this order
public interface IScriptRegistry
{
    // assigns ComponentType<T>.Index for each [Component] from `firstIndex` up
    ComponentDescriptor[] RegisterComponents(int firstIndex);

    // the [System]s outside of scene scripts, those register with their script
    SystemDescriptor[] CreateSystems();

    // also assigns BehaviourType<T>.Name for each behaviour
    ScriptType[] CreateScriptTypes();

    void OnLoad();
}
