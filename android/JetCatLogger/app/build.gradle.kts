plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.usaheliman.jetcatlogger"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.usaheliman.jetcatlogger"
        minSdk = 26
        targetSdk = 35
        // Version: "dev-<commit>" for every build, except a release made by the GitHub
        // "Release" action, which passes the number Mehul types in (-PappVersion=1.0.0).
        // versionCode = minutes since 1 Jan 2026, so every build installs over an older one
        // (dev or release) and the update check can tell which is newer.
        val appVersion = (project.findProperty("appVersion") as String?)?.takeIf { it.isNotBlank() }
        versionCode = ((System.currentTimeMillis() - 1767225600000L) / 60000L).toInt()
        versionName = appVersion ?: ("dev-" + (project.findProperty("gitSha") as String? ?: "local"))
        buildConfigField("String", "ONEDRIVE_CLIENT_ID",
            "\"" + (project.findProperty("ONEDRIVE_CLIENT_ID") as String? ?: "") + "\"")
        // Releases (app + logger firmware) are published on GitHub; the app reads this list.
        buildConfigField("String", "RELEASES_API",
            "\"https://api.github.com/repos/USAheliman/jetcat-ecu-logger/releases\"")
    }
    // Signing key: kept out of the (public) repo. GitHub builds get it from the repository
    // secret APP_KEYSTORE_B64 (-PsigningStore=<file> -PsigningPassword=...), so every
    // published build installs over the previous one and keeps the flights on the phone.
    // A local build without it uses Android's debug key (fine for testing on a cable).
    val signingStore = project.findProperty("signingStore") as String?
    signingConfigs {
        if (signingStore != null) create("jetcat") {
            storeFile = file(signingStore)
            storePassword = project.findProperty("signingPassword") as String?
            keyAlias = "jetcat"
            keyPassword = project.findProperty("signingPassword") as String?
            storeType = "pkcs12"
        }
    }
    buildTypes {
        debug { if (signingStore != null) signingConfig = signingConfigs.getByName("jetcat") }
        release { isMinifyEnabled = false; if (signingStore != null) signingConfig = signingConfigs.getByName("jetcat") }
    }
    buildFeatures { buildConfig = true }
    // The pages are the logger's own: data/index.html at the top of the repo.
    sourceSets["main"].assets.srcDirs("../../../data")
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    testImplementation("junit:junit:4.13.2")
}
