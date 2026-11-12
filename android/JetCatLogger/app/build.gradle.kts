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
        // Version numbers are Mehul's to bump once a build is proven.
        versionCode = 1
        versionName = "dev"
        buildConfigField("String", "ONEDRIVE_CLIENT_ID",
            "\"" + (project.findProperty("ONEDRIVE_CLIENT_ID") as String? ?: "") + "\"")
    }
    // One fixed signing key (keystore/jetcat-logger.p12, kept in this private repo), so
    // every build installs over the previous one and keeps the flights stored on the phone.
    signingConfigs {
        create("jetcat") {
            storeFile = file("../keystore/jetcat-logger.p12")
            storePassword = "jetcatlogger"
            keyAlias = "jetcat"
            keyPassword = "jetcatlogger"
            storeType = "pkcs12"
        }
    }
    buildTypes {
        debug { signingConfig = signingConfigs.getByName("jetcat") }
        release { isMinifyEnabled = false; signingConfig = signingConfigs.getByName("jetcat") }
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
