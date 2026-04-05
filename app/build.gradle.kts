plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val appVersionCode = 30
val appVersionName = "0.3.16-end-to-end-recovery-2026-04-05"

android {
    namespace = "com.example.androidcast"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.example.androidcast"
        minSdk = 29
        targetSdk = 36
        versionCode = appVersionCode
        versionName = appVersionName

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.activity:activity-ktx:1.9.2")
    implementation("androidx.lifecycle:lifecycle-service:2.8.5")
}

val copyVersionedDebugApk by tasks.registering(Copy::class) {
    from(layout.buildDirectory.file("outputs/apk/debug/app-debug.apk"))
    into(layout.buildDirectory.dir("outputs/versioned-apk/debug"))
    rename { "z6y-live-cast-assistant-${appVersionName}-debug.apk" }
    onlyIf { layout.buildDirectory.file("outputs/apk/debug/app-debug.apk").get().asFile.exists() }
}

afterEvaluate {
    tasks.named("assembleDebug") {
        finalizedBy(copyVersionedDebugApk)
    }
}
