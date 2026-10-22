
#include "Includes/obfuscate.h"
#include "Menu/Menu.hpp"

bool iconValid = false;
bool settingsValid = false;
bool featuresValid = false;
bool initValid = false;

//Big letter cause crash
void setText(JNIEnv *env, jobject obj, const char* text){
    //https://stackoverflow.com/a/33627640/3763113
    //A little JNI calls here. You really really need a great knowledge if you want to play with JNI stuff
    //Html.fromHtml("");
    jclass html = (*env).FindClass(OBFUSCATE("android/text/Html"));
    jmethodID fromHtml = (*env).GetStaticMethodID(html, OBFUSCATE("fromHtml"), OBFUSCATE("(Ljava/lang/String;)Landroid/text/Spanned;"));

    //setText("");
    jclass textView = (*env).FindClass(OBFUSCATE("android/widget/TextView"));
    jmethodID setText = (*env).GetMethodID(textView, OBFUSCATE("setText"), OBFUSCATE("(Ljava/lang/CharSequence;)V"));

    //Java string
    jstring jstr = (*env).NewStringUTF(text);
    (*env).CallVoidMethod(obj, setText,  (*env).CallStaticObjectMethod(html, fromHtml, jstr));
}

jstring Icon(JNIEnv *env, jobject thiz) {

    //Use https://www.base64encode.org/ to encode your image to base64
  return env->NewStringUTF(
           OBFUSCATE("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"));
}

jstring IconWebViewData(JNIEnv *env, jobject thiz) {
        

    //WebView support GIF animation. Upload your image or GIF on imgur.com or other sites

    // From internet (Requires android.permission.INTERNET)
    // return env->NewStringUTF(OBFUSCATE("https://i.imgur.com/SujJ85j.gif"));

    // Base64 html:
    // return env->NewStringUTF("data:image/png;base64, <encoded base64 here>");

    // To disable it, return nullptr. It will use normal image above:
    // return nullptr

    //return env->NewStringUTF(OBFUSCATE_KEY("https://i.imgur.com/SujJ85j.gif", 'u'));
    return nullptr;
}

jobjectArray SettingsList(JNIEnv *env, jobject activityObject) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Category_Settings"),
            OBFUSCATE("-1_Toggle_Save feature preferences"), //-1 is checked on Preferences.java
            OBFUSCATE("-3_Toggle_Auto size vertically"),
            OBFUSCATE("Category_Menu"),
            OBFUSCATE("-6_Button_<font color='red'>Close settings</font>"),
    };

    int Total_Feature = (sizeof features /
                         sizeof features[0]); //Now you dont have to manually update the number everytime;
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));
    int i;
    for (i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return (ret);
}
