// Copyright Convai. All Rights Reserved.

using System;
using System.IO;
using System.Text;
#if UE_5_5_OR_LATER
using System.Security.Cryptography;
#endif
#if UE_5_1_OR_LATER
using System.Text.RegularExpressions;
#endif
using UnrealBuildTool;

public class ConvaiSceneAutoTaggerCore : ModuleRules
{
	public ConvaiSceneAutoTaggerCore(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		string LockFile = Path.Combine(ModuleDirectory, "core.lock.json");
		if (!File.Exists(LockFile))
		{
			throw new BuildException(
				"Scene Auto Tagger native core lock file is missing from the pinned ThirdParty package: {0}",
				LockFile);
		}
		VerifyLockMetadata(LockFile);

		string Header = Path.Combine(ModuleDirectory, "include", "convai_scene_auto_tagger_core.h");
		if (!File.Exists(Header))
		{
			throw new BuildException(
				"Scene Auto Tagger native core header is missing from the pinned ThirdParty package: {0}",
				Header);
		}
		VerifyPinnedTextArtifact(LockFile, "include/convai_scene_auto_tagger_core.h", Header);
		PublicIncludePaths.Add(Path.GetDirectoryName(Header));

		string CoreDll = Path.Combine(
			ModuleDirectory,
			"Win64",
			"bin",
			"convai_scene_auto_tagger_core.dll");
		string HelperDll = Path.Combine(
			ModuleDirectory,
			"Win64",
			"bin",
			"convai_http_helper.dll");
		string CoreDylib = Path.Combine(
			ModuleDirectory,
			"Mac",
			"lib",
			"libconvai_scene_auto_tagger_core.dylib");
		string HelperDylib = Path.Combine(
			ModuleDirectory,
			"Mac",
			"lib",
			"libconvai_http_helper.dylib");
		string CoreSo = Path.Combine(
			ModuleDirectory,
			"Linux",
			"x86_64-unknown-linux-gnu",
			"lib",
			"libconvai_scene_auto_tagger_core.so");
		string HelperSo = Path.Combine(
			ModuleDirectory,
			"Linux",
			"x86_64-unknown-linux-gnu",
			"lib",
			"libconvai_http_helper.so");
		VerifyRequiredPinnedArtifact(
			LockFile,
			"Win64/bin/convai_scene_auto_tagger_core.dll",
			CoreDll,
			"Win64 core DLL");
		VerifyRequiredPinnedArtifact(
			LockFile,
			"Win64/bin/convai_http_helper.dll",
			HelperDll,
			"Win64 HTTP helper DLL");
		VerifyRequiredPinnedArtifact(
			LockFile,
			"Mac/lib/libconvai_scene_auto_tagger_core.dylib",
			CoreDylib,
			"macOS portability library");
		VerifyRequiredPinnedArtifact(
			LockFile,
			"Mac/lib/libconvai_http_helper.dylib",
			HelperDylib,
			"macOS portability HTTP helper library");
		VerifyRequiredPinnedArtifact(
			LockFile,
			"Linux/x86_64-unknown-linux-gnu/lib/libconvai_scene_auto_tagger_core.so",
			CoreSo,
			"Linux portability library");
		VerifyRequiredPinnedArtifact(
			LockFile,
			"Linux/x86_64-unknown-linux-gnu/lib/libconvai_http_helper.so",
			HelperSo,
			"Linux portability HTTP helper library");
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDefinitions.Add("WITH_CONVAI_SCENE_AUTO_TAGGER_CORE=1");
			string DllName = Path.GetFileName(CoreDll);
			RuntimeDependencies.Add(
				Path.Combine(
					PluginDirectory,
					"Binaries",
					"ThirdParty",
					"ConvaiSceneAutoTaggerCore",
					"Win64",
					DllName),
				CoreDll,
				StagedFileType.NonUFS);
			RuntimeDependencies.Add(
				"$(TargetOutputDir)/" + DllName,
				CoreDll,
				StagedFileType.NonUFS);
			RuntimeDependencies.Add(
				"$(ProjectDir)/Binaries/Win64/" + DllName,
				CoreDll,
				StagedFileType.NonUFS);
			// The core resolves convai_http_helper by absolute path from its own
			// directory, so the helper is staged only beside the core copy the
			// adapter actually loads. The shared output directories already carry
			// ConvaiWebRTC's same-named helper; staging there would collide.
			RuntimeDependencies.Add(
				Path.Combine(
					PluginDirectory,
					"Binaries",
					"ThirdParty",
					"ConvaiSceneAutoTaggerCore",
					"Win64",
					Path.GetFileName(HelperDll)),
				HelperDll,
				StagedFileType.NonUFS);
		}
		else
		{
			PublicDefinitions.Add("WITH_CONVAI_SCENE_AUTO_TAGGER_CORE=0");
		}
	}

	private static void VerifyLockMetadata(string LockFile)
	{
		string LockText = File.ReadAllText(LockFile);
		if (MatchPattern(
			LockText,
			"\\\"schema_version\\\"\\s*:\\s*3(?:\\s*[,}])",
			0) == null)
		{
			throw new BuildException(
				"Scene Auto Tagger native core lock file must use dependency schema 3: {0}",
				LockFile);
		}

		string CoreVersion = RequireStringField(
			LockText,
			"core_version",
			"[0-9]+\\.[0-9]+\\.[0-9]+",
			LockFile);
		string ReleaseTag = RequireStringField(
			LockText,
			"release_tag",
			"v[0-9]+\\.[0-9]+\\.[0-9]+",
			LockFile);
		if (!String.Equals(ReleaseTag, "v" + CoreVersion, StringComparison.Ordinal))
		{
			throw new BuildException(
				"Scene Auto Tagger native core release tag does not match core_version in {0}",
				LockFile);
		}

		RequireStringField(LockText, "source_commit", "[0-9A-Fa-f]{40}", LockFile);
		string ReleaseChannel = RequireStringField(
			LockText,
			"release_channel",
			"(?:local-prerelease|unsigned-internal-prerelease|signed-release)",
			LockFile);
		RequireStringField(
			LockText,
			"expected_entry_point",
			"convai_sat_get_api",
			LockFile);

		// A local prerelease is vendored straight from a workstation build; no
		// published archives exist to pin. Every bundled file is still verified.
		if (String.Equals(ReleaseChannel, "local-prerelease", StringComparison.Ordinal))
		{
			return;
		}

		RequireReleaseArchive(
			LockText,
			"windows-x86_64",
			String.Format("convai-scene-auto-tagger-core-v{0}-windows-x86_64.zip", CoreVersion),
			"unreal-integrated",
			true,
			LockFile);
		RequireReleaseArchive(
			LockText,
			"linux-x86_64",
			String.Format("convai-scene-auto-tagger-core-v{0}-linux-x86_64.tar.gz", CoreVersion),
			"portability-validated",
			false,
			LockFile);
		RequireReleaseArchive(
			LockText,
			"macos-universal",
			String.Format("convai-scene-auto-tagger-core-v{0}-macos-universal.tar.gz", CoreVersion),
			"portability-validated",
			false,
			LockFile);
	}

	private static void RequireReleaseArchive(
		string LockText,
		string TargetId,
		string ExpectedArchiveName,
		string ExpectedSupportLevel,
		bool ExpectedUnrealEligibility,
		string LockFile)
	{
		string ArchivePattern = String.Format(
			"\\\"{0}\\\"\\s*:\\s*\\{{[^{{}}]*"
				+ "\\\"name\\\"\\s*:\\s*\\\"{1}\\\"[^{{}}]*"
				+ "\\\"sha256\\\"\\s*:\\s*\\\"[0-9A-Fa-f]{{64}}\\\"[^{{}}]*"
				+ "\\\"workflow_run\\\"\\s*:\\s*[1-9][0-9]*[^{{}}]*"
				+ "\\\"support_level\\\"\\s*:\\s*\\\"{2}\\\"[^{{}}]*"
				+ "\\\"unreal_release_eligible\\\"\\s*:\\s*{3}[^{{}}]*\\}}",
			EscapePattern(TargetId),
			EscapePattern(ExpectedArchiveName),
			EscapePattern(ExpectedSupportLevel),
			ExpectedUnrealEligibility ? "true" : "false");
		if (MatchPattern(LockText, ArchivePattern, 0) == null)
		{
			throw new BuildException(
				"Scene Auto Tagger native core lock file has no valid {0} archive identity: {1}",
				TargetId,
				LockFile);
		}
	}

	private static string RequireStringField(
		string LockText,
		string FieldName,
		string ValuePattern,
		string LockFile)
	{
		string Pattern = String.Format(
			"\\\"{0}\\\"\\s*:\\s*\\\"({1})\\\"",
			EscapePattern(FieldName),
			ValuePattern);
		string FieldMatch = MatchPattern(LockText, Pattern, 1);
		if (FieldMatch == null)
		{
			throw new BuildException(
				"Scene Auto Tagger native core lock file has no valid {0}: {1}",
				FieldName,
				LockFile);
		}
		return FieldMatch;
	}

	private static void VerifyPinnedArtifact(
		string LockFile,
		string ArtifactKey,
		string ArtifactPath)
	{
		string LockText = File.ReadAllText(LockFile);
		string ArtifactPattern = String.Format(
			"\\\"{0}\\\"\\s*:\\s*\\{{[^{{}}]*\\\"sha256\\\"\\s*:\\s*\\\"([0-9A-Fa-f]{{64}})\\\"",
			EscapePattern(ArtifactKey));
		string HashMatch = MatchPattern(LockText, ArtifactPattern, 1);
		if (HashMatch == null)
		{
			throw new BuildException(
				"Scene Auto Tagger native core lock file has no valid SHA-256 for {0}: {1}",
				ArtifactKey,
				LockFile);
		}

		string ActualHash;
		using (FileStream ArtifactStream = File.OpenRead(ArtifactPath))
		{
			ActualHash = ComputeSha256(ArtifactStream);
		}

		string ExpectedHash = HashMatch;
		if (!String.Equals(ActualHash, ExpectedHash, StringComparison.OrdinalIgnoreCase))
		{
			throw new BuildException(
				"Scene Auto Tagger native core artifact does not match core.lock.json: {0}",
				ArtifactPath);
		}
	}

	private static void VerifyPinnedTextArtifact(
		string LockFile,
		string ArtifactKey,
		string ArtifactPath)
	{
		string LockText = File.ReadAllText(LockFile);
		string ArtifactPattern = String.Format(
			"\\\"{0}\\\"\\s*:\\s*\\{{[^\\{{\\}}]*\\\"sha256\\\"\\s*:\\s*\\\"([0-9A-Fa-f]{{64}})\\\"",
			EscapePattern(ArtifactKey));
		string HashMatch = MatchPattern(LockText, ArtifactPattern, 1);
		if (HashMatch == null)
		{
			throw new BuildException(
				"Scene Auto Tagger native core lock file has no valid SHA-256 for {0}: {1}",
				ArtifactKey,
				LockFile);
		}

		// Perforce and Git may materialize text files with platform line endings.
		// Hash the canonical UTF-8/LF representation used by the release archive.
		string CanonicalText = File.ReadAllText(ArtifactPath)
			.Replace("\r\n", "\n")
			.Replace("\r", "\n");
		string ActualHash;
		using (MemoryStream ArtifactStream = new MemoryStream(Encoding.UTF8.GetBytes(CanonicalText)))
		{
			ActualHash = ComputeSha256(ArtifactStream);
		}

		if (!String.Equals(ActualHash, HashMatch, StringComparison.OrdinalIgnoreCase))
		{
			throw new BuildException(
				"Scene Auto Tagger native core text artifact does not match core.lock.json: {0}",
				ArtifactPath);
		}
	}

	// UE 5.0's rules compiler omits the Regex and HashAlgorithm reference assemblies.
	// Its runtime still supplies these APIs. Resolve them there only on 5.0, keeping
	// the exact same regex options and SHA-256 checks (including failure behavior).
	private static string EscapePattern(string Value)
	{
#if UE_5_1_OR_LATER
		return Regex.Escape(Value);
#else
		System.Type RegexType = System.Type.GetType("System.Text.RegularExpressions.Regex, System.Text.RegularExpressions", true);
		return (string)RegexType.GetMethod("Escape", new[] { typeof(string) }).Invoke(null, new object[] { Value });
#endif
	}

	private static string MatchPattern(string Text, string Pattern, int GroupIndex)
	{
#if UE_5_1_OR_LATER
		Match Result = Regex.Match(Text, Pattern, RegexOptions.CultureInvariant);
		return Result.Success ? Result.Groups[GroupIndex].Value : null;
#else
		System.Type RegexType = System.Type.GetType("System.Text.RegularExpressions.Regex, System.Text.RegularExpressions", true);
		Type OptionsType = RegexType.Assembly.GetType("System.Text.RegularExpressions.RegexOptions", true);
		object Options = Enum.Parse(OptionsType, "CultureInvariant");
		object Result = RegexType.GetMethod("Match", new[] { typeof(string), typeof(string), OptionsType })
			.Invoke(null, new object[] { Text, Pattern, Options });
		if (!(bool)Result.GetType().GetProperty("Success").GetValue(Result))
		{
			return null;
		}
		object Groups = Result.GetType().GetProperty("Groups").GetValue(Result);
		object Group = Groups.GetType().GetProperty("Item", new[] { typeof(int) })
			.GetValue(Groups, new object[] { GroupIndex });
		return (string)Group.GetType().GetProperty("Value").GetValue(Group);
#endif
	}

	private static string ComputeSha256(Stream ArtifactStream)
	{
#if UE_5_5_OR_LATER
		using (SHA256 Hasher = SHA256.Create())
		{
			return BitConverter.ToString(Hasher.ComputeHash(ArtifactStream)).Replace("-", "");
		}
#else
		// UE 5.3/5.4's .NET 6 rules compiler also omits HashAlgorithm's reference
		// assembly. Runtime resolution retains the same SHA-256 verification.
		System.Type HashType = System.Type.GetType("System.Security.Cryptography.SHA256, System.Security.Cryptography.Algorithms", true);
		using (IDisposable Hasher = (IDisposable)HashType.GetMethod("Create", System.Type.EmptyTypes).Invoke(null, null))
		{
			byte[] Hash = (byte[])HashType.GetMethod("ComputeHash", new[] { typeof(Stream) })
				.Invoke(Hasher, new object[] { ArtifactStream });
			return BitConverter.ToString(Hash).Replace("-", "");
		}
#endif
	}

	private static void VerifyRequiredPinnedArtifact(
		string LockFile,
		string ArtifactKey,
		string ArtifactPath,
		string ArtifactDescription)
	{
		if (!File.Exists(ArtifactPath))
		{
			throw new BuildException(
				"Scene Auto Tagger {0} is missing from the pinned desktop package: {1}",
				ArtifactDescription,
				ArtifactPath);
		}
		VerifyPinnedArtifact(LockFile, ArtifactKey, ArtifactPath);
	}
}
