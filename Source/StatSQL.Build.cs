namespace UnrealBuildTool.Rules
{
    public class StatSQL : ModuleRules
    {
        public StatSQL(TargetInfo Target)
        {
            PrivateIncludePaths.Add("StatSQL/Private");
            PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

            PublicIncludePaths.AddRange(new string[] {
                "StatSQL/Public"
            });

            PrivateIncludePaths.AddRange(new string[] {
                "UnrealTournament/Private",
                "UnrealTournament/Classes"
            });

            PublicDependencyModuleNames.AddRange(
                new string[]
                {
                    "Core",
                    "CoreUObject",
                    "Engine",
                    "UnrealTournament",
                    "InputCore",
                    "Http",
                    "Json",
                    "JsonUtilities",
                }
            );
        }
    }
}
