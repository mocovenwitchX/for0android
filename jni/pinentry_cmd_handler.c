
#include <stdlib.h>
#include <jni.h>

#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/un.h>
#include <locale.h>
#include <pwd.h>

#include <android/log.h>

#include "pinentry.h"

#define GPG_APP_PATH "/data/data/info.guardianproject.gpg"

#define SOCKET_PINENTRY "info.guardianproject.gpg.pinentry"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG , "PINENTRY", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR , "PINENTRY", __VA_ARGS__)

int recv_fd ( int socket )
{
    LOGD("recv_fd");
    int sent_fd;
    struct msghdr socket_message;
    struct iovec io_vector[1];
    struct cmsghdr* control_message = NULL;
    char message_buffer[1];
    char ancillary_element_buffer[CMSG_SPACE ( sizeof ( int ) )];

    /* start clean */
    memset ( &socket_message, 0, sizeof ( struct msghdr ) );
    memset ( ancillary_element_buffer, 0, CMSG_SPACE ( sizeof ( int ) ) );

    /* setup a place to fill in message contents */
    io_vector[0].iov_base = message_buffer;
    io_vector[0].iov_len = 1;
    socket_message.msg_iov = io_vector;
    socket_message.msg_iovlen = 1;

    /* provide space for the ancillary data */
    socket_message.msg_control = ancillary_element_buffer;
    socket_message.msg_controllen = CMSG_SPACE ( sizeof ( int ) );

    if ( recvmsg ( socket, &socket_message,  0) < 0 )
        return -1;

    if ( message_buffer[0] != 'F' ) {
        /* this did not originate from the above function */
        return -1;
    }

    if ( ( socket_message.msg_flags & MSG_CTRUNC ) == MSG_CTRUNC ) {
        /* we did not provide enough space for the ancillary element array */
        return -1;
    }

    /* iterate ancillary elements */
    for ( control_message = CMSG_FIRSTHDR ( &socket_message );
            control_message != NULL;
            control_message = CMSG_NXTHDR ( &socket_message, control_message ) ) {
        if ( ( control_message->cmsg_level == SOL_SOCKET ) &&
                ( control_message->cmsg_type == SCM_RIGHTS ) ) {
            sent_fd = * ( ( int* ) CMSG_DATA ( control_message ) );
            return sent_fd;
        }
    }

    return -1;
}

/* static variables for JNI voodoo */
struct pe_context _ctx;

/* static variables for pinentry voodoo */
static pinentry_t pinentry;
static int passphrase_ok;
typedef enum { CONFIRM_CANCEL, CONFIRM_OK, CONFIRM_NOTOK } confirm_value_t;
static confirm_value_t confirm_value;

struct pe_context {
    JavaVM* jvm;
    JNIEnv* env;

    pinentry_t pe;

    // "info.guardianproject.gpg.pinentry.PinentryStruct"
    jclass  pe_struct_class;
    jobject pe_struct;

    // "info.guardianproject.gpg.pinentry.PinentryDialog"
    jclass  pe_activity_class;
    jobject pe_activity;

    char internal_gnupghome[UNIX_PATH_MAX];
};

int pe_get_internal_gnupghome( struct pe_context* ctx )
{
    LOGD("pe_get_internal_gnupghome");
    jclass NativeHelperCls = ( *ctx->env )->FindClass ( ctx->env, "info/guardianproject/gpg/NativeHelper" );
    if ( !NativeHelperCls ) {
        LOGE ( "pe_get_internal_gnupghome: failed to retrieve NativeHelperCls\n" );
        return -1;
    }

    jfieldID fid_app_home = (*ctx->env)->GetStaticFieldID( ctx->env, NativeHelperCls, "app_home", "Ljava/io/File;");
    if ( fid_app_home == 0 ) {
        LOGE( "pe_get_internal_gnupghome: failed to get fid_app_home" );
        return -1;
    }

    jclass FileCls = ( *ctx->env )->FindClass ( ctx->env, "java/io/File" );
    if ( !FileCls ) {
        LOGE ( "pe_get_internal_gnupghome: failed to retrieve FileCls\n" );
        return -1;
    }

    jobject obj_File_app_home = (*ctx->env)->GetStaticObjectField( ctx->env, NativeHelperCls, fid_app_home );
    if ( obj_File_app_home == 0 ) {
        LOGE( "pe_get_internal_gnupghome: failed to get obj_File_app_home" );
        return -1;
    }

    jmethodID method_File_getAbsolutePath = ( *ctx->env )->GetMethodID ( ctx->env, FileCls, "getAbsolutePath", "()Ljava/lang/String;" );
    if ( method_File_getAbsolutePath == 0 ) {
        LOGE( "pe_get_internal_gnupghome: failed to get method_File_getAbsolutePath" );
        return -1;
    }

    jstring absolutePath = ( *ctx->env )->CallObjectMethod ( ctx->env, obj_File_app_home, method_File_getAbsolutePath );
    if ( method_File_getAbsolutePath == 0 ) {
        LOGE( "pe_get_internal_gnupghome: failed to CallObjectMethod(method_File_getAbsolutePath)" );
        return -1;
    }

    jsize gnupghome_len = ( *ctx->env )->GetStringUTFLength ( ctx->env, absolutePath );
    const jbyte* gnupghome = ( *ctx->env )->GetStringUTFChars ( ctx->env, absolutePath, 0 );

    if ( gnupghome_len > UNIX_PATH_MAX ) {
        LOGE("pe_get_internal_gnupghome: gnupghome_len > UNIX_PATH_MAX (%d > %d)", gnupghome_len, UNIX_PATH_MAX);
        return -1;
    }
    strncpy(ctx->internal_gnupghome, gnupghome, gnupghome_len);

    ( *ctx->env )->ReleaseStringUTFChars ( ctx->env, absolutePath, gnupghome );
    return 0;
}
int pe_context_init( struct pe_context* ctx, JavaVM* vm )
{
    LOGD("pe_context_init");
    ctx->jvm = vm;
    if ( ctx->jvm == 0 ) {
        LOGE ( "pe_context_init: JVM is null\n" );
        return -1;
    }

    // attach to JVM
    if ( JNI_OK != ( *ctx->jvm )->AttachCurrentThread ( ctx->jvm, &ctx->env, 0 ) ) {
        LOGE( "pe_context_init: AttachCurrentThread failed");
        return -1;
    }

    ctx->pe_activity = 0;
    ctx->pe_activity_class = 0;
    ctx->pe_struct = 0;
    ctx->pe_struct_class = 0;
    ctx->env = 0;
    return 0;
}

int pe_struct_init( struct pe_context* ctx )
{
    LOGD("pe_struct_init");

    ctx->pe_struct_class = ( *ctx->env )->FindClass ( ctx->env, "info/guardianproject/gpg/pinentry/PinentryStruct" );

    if ( !ctx->pe_struct_class ) {
        LOGE ( "pe_struct_init: failed to retrieve PinentryStruct\n" );
        return -1;
    }

    jmethodID constructor = ( *ctx->env )->GetMethodID ( ctx->env, ctx->pe_struct_class, "<init>", "()V" );
    ctx->pe_struct = ( *ctx->env )->NewObject ( ctx->env, ctx->pe_struct_class, constructor );
    if ( !ctx->pe_struct )  {
        LOGE ( "pe_struct_init: NewObject failed\n" );
        return -1;
    }
    return 0;
}

jstring pe_new_string ( const struct pe_context* ctx, const char* field, const char* value )
{
    jstring jString = (*ctx->env)->NewStringUTF(ctx->env, value);
    if (jString == 0) {
        LOGE( "pe_new_string: failed to create str %s with %s\n", field, value );
        return 0;
    }
    return jString;
}

int pe_set_str(const struct pe_context* ctx,  const char* field, jstring value)
{
    jfieldID fid;

    fid = (*ctx->env)->GetFieldID( ctx->env, ctx->pe_struct_class, field, "Ljava/lang/String;");
    if ( fid == 0 ) {
        LOGE( "pe_set_str: failed to get fid %s", field );
        return -1;
    }
    (*ctx->env)->SetObjectField( ctx->env, ctx->pe_struct, fid, value );
    return 0;
}

int pe_set_int(const struct pe_context* ctx,  const char* field, int value)
{
    jfieldID fid;

    fid = (*ctx->env)->GetFieldID( ctx->env, ctx->pe_struct_class, field, "I");
    if ( fid == 0 ) {
        LOGE( "pe_set_int: failed to get fid %s", field );
        return -1;
    }
    (*ctx->env)->SetIntField( ctx->env, ctx->pe_struct, fid, value );
    return 0;
}

int pe_add_string( const struct pe_context* ctx, const char* field, const char* value )
{
    if ( value ) {
        jstring jField = pe_new_string( ctx, field, value );
        if ( !jField ) {
            LOGE("pe_add_string: no such field, %s", field);
            return -1;
        }
        return pe_set_str( ctx, field, jField );
    }
    return 0;
}

int pe_activity_init( struct pe_context* ctx, jobject pe_activity )
{
    LOGD("pe_activity_init");
    ctx->pe_activity_class = ( *ctx->env )->FindClass ( ctx->env, "info/guardianproject/gpg/pinentry/PinentryDialog" );
    if ( !ctx->pe_activity_class ) {
        LOGE ( "pe_activity_init: failed to retrieve PinentryDialog.class\n" );
        return -1;
    }

    ctx->pe_activity = pe_activity;
    if ( !ctx->pe_activity ) {
        LOGE ( "pe_activity_init: PinentryDialog null\n" );
        return -1;
    }
    return 0;
}

int pe_set_pe_struct( struct pe_context* ctx )
{
    LOGD("pe_set_pe_struct");
    jobject result;
    jmethodID setPinentryStructMethod;
    setPinentryStructMethod = ( *ctx->env )->GetMethodID ( ctx->env, ctx->pe_activity_class, "setPinentryStruct", "(Linfo/guardianproject/gpg/pinentry/PinentryStruct;)Linfo/guardianproject/gpg/pinentry/PinentryStruct;" );
    if ( !setPinentryStructMethod ) {
        LOGE ( "pe_set_pe_struct: failed to retrieve setPinentryStructMethod\n" );
        return -1;
    }
    result = ( *ctx->env )->CallObjectMethod ( ctx->env, ctx->pe_activity, setPinentryStructMethod, ctx->pe_struct );
    if ( !result ) {
        LOGE ( "pe_set_pe_struct: result is null!!\n" );
        return -1;
    }
    ctx->pe_struct = result;
    return 0;
}

/*
 * returns the pin length, or -1 on error
 */
int pe_get_pin( const struct pe_context* ctx )
{
    jfieldID fid;
    fid = ( *ctx->env )->GetFieldID ( ctx->env, ctx->pe_struct_class , "pin", "Ljava/lang/String;" );
    if ( !fid ) {
        LOGE ( "pe_get_pin: failed to get pin jfieldID\n" );
        return -1;
    }

    jstring jpin = ( *ctx->env )->GetObjectField ( ctx->env, ctx->pe_struct, fid );
    if ( !jpin ) {
        LOGE ( "pe_get_pin: jpin is null!!\n" );
        return -1;
    }

    jsize pin_len = ( *ctx->env )->GetStringUTFLength ( ctx->env, jpin );
    const jbyte* pin = ( *ctx->env )->GetStringUTFChars ( ctx->env, jpin, 0 );

    if ( pin_len <= 0 ) {
        LOGE( "pe_get_pin: pin_len <=0 " );
        goto pin_error;
    }

    pinentry_setbufferlen ( ctx->pe, pin_len + 1 );
    if ( !ctx->pe->pin ) {
        LOGE( "pe_get_pin: error allocating pin buffer" );
        goto pin_error;
    }

    strncpy ( ctx->pe->pin, pin, pin_len );

    ( *ctx->env )->ReleaseStringUTFChars ( ctx->env, jpin, pin );
    return pin_len;

pin_error:
    ( *ctx->env )->ReleaseStringUTFChars ( ctx->env, jpin, pin );
    return -1;
}

int pe_get_result( const struct pe_context* ctx )
{
    jfieldID fid;
    fid = ( *ctx->env )->GetFieldID ( ctx->env, ctx->pe_struct_class , "result", "I" );
    if ( !fid ) {
        LOGE ( "pe_get_result: failed to get result jfieldID\n" );
        return -1;
    }

    jint result = ( *ctx->env )->GetIntField ( ctx->env, ctx->pe_struct, fid );
    ctx->pe->result = result;
    return 0;
}

int pe_get_canceled( const struct pe_context* ctx )
{
    LOGD("pe_get_canceled");
    jfieldID fid;
    fid = ( *ctx->env )->GetFieldID ( ctx->env, ctx->pe_struct_class , "canceled", "I" );
    if ( !fid ) {
        LOGE ( "pe_get_canceled: failed to get canceled jfieldID\n" );
        return -1;
    }

    jint canceled = ( *ctx->env )->GetIntField ( ctx->env, ctx->pe_struct, fid );
    ctx->pe->canceled = canceled;
    return 0;
}

int pe_fill_data( struct pe_context* ctx )
{
    LOGD("pe_fill_data");
    int rc = 0;
    // populate the PinentryStruct with values from pinentry_t pe
    rc = pe_add_string( ctx, "title", ctx->pe->title );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "description", ctx->pe->description );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "prompt", ctx->pe->prompt );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "error", ctx->pe->error );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "ok", ctx->pe->ok );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "default_ok", ctx->pe->default_ok );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "cancel", ctx->pe->cancel );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "default_cancel", ctx->pe->default_cancel );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "quality_bar", ctx->pe->quality_bar );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "quality_bar_tt", ctx->pe->quality_bar_tt );
    if ( rc < 0 ) return -1;
    rc = pe_add_string( ctx, "notok", ctx->pe->notok );
    if ( rc < 0 ) return -1;

    rc = pe_set_int( ctx, "one_button", ctx->pe->one_button );
    if ( rc < 0 ) return -1;
    rc = pe_set_int( ctx, "timeout", ctx->pe->timeout );
    if ( rc < 0 ) return -1;

    return 0;
}

/*
 * JNI voodoo to drive the Android GUI
 * create the pin prompt and fetch back the user's input
 * blocks until user enters pin or it is canceled
 */
int pe_prompt_pin ( void )
{
    LOGD("pe_prompt_pin");
    int rc = 0;

    // instantiates the Java PinentryStruct object
    rc = pe_struct_init( &_ctx );
    if ( rc < 0 ) return -1;

    rc = pe_fill_data( &_ctx );
    if ( rc < 0 ) return -1;

    // call PinentryDialog.setPinentryStruct() to set PinentryStruct we made
    //    note â this function blocks until the users enters a pin, cancels, or the PinentryDialog is closed
    rc = pe_set_pe_struct( &_ctx );
    if ( rc < 0 ) return -1;

    pe_get_result( &_ctx );
    pe_get_canceled( &_ctx );

    // fetches the user supplied pin from Java (if there is one!)
    return pe_get_pin( &_ctx );
}

int pe_prompt_buttons ( void )
{
    LOGD("pe_prompt_buttons");
    int rc = 0;
    // instantiates the Java PinentryStruct object
    rc = pe_struct_init( &_ctx );
    if ( rc < 0 ) return -1;

    rc = pe_fill_data( &_ctx );
    if ( rc < 0 ) return -1;

    rc = pe_set_int( &_ctx, "isButtonBox", 0 ); // true
    if ( rc < 0 ) return -1;

    // call PinentryDialog.setPinentryStruct() to set PinentryStruct we made
    //    note â this function blocks until the user clicks a button or the PinentryDialog is closed
    rc = pe_set_pe_struct( &_ctx );
    if ( rc < 0 ) return -1;

    pe_get_result( &_ctx );
    pe_get_canceled( &_ctx );

    return 0;
}

static int
android_cmd_handler ( pinentry_t pe )
{
    LOGD("android_cmd_handler");
    int want_pass = !!pe->pin;

    pinentry = pe;
    confirm_value = CONFIRM_CANCEL;
    passphrase_ok = 0;

    if ( confirm_value == CONFIRM_CANCEL )
        pe->canceled = 1;

    pinentry = NULL;
    _ctx.pe = pe;
    if ( want_pass ) {
        LOGD ( "android_cmd_handler: i think they want a pin..\n" );
        return pe_prompt_pin ();
    } else {
        LOGE("android_cmd_handler: we don't do this");
        return pe_prompt_buttons();
    }
}

pinentry_cmd_handler_t pinentry_cmd_handler = android_cmd_handler;


/*
 * connect to the pinetry helper over a unix domain socket
 */
int connect_helper( int app_uid )
{
    LOGD("connect_helper");
    struct sockaddr_un addr;
    char path[UNIX_PATH_MAX];
    int path_len = 0;
    int fd;

    if ( ( fd = socket ( AF_UNIX, SOCK_STREAM, 0 ) ) == -1 ) {
        LOGE ( "connect_helper: socket error" );
        return -1;
    }

    if ( app_uid == getuid() ) {
        // we're an internal pinentry, so use a file-backed socket
        path_len = snprintf( path, sizeof( path ), "%s/S.pinentry", _ctx.internal_gnupghome );
    } else {
        // we're an external pinentry, so use an abstract socket
        path_len = snprintf( &path[1], sizeof( path ), "%s.%d", SOCKET_PINENTRY, app_uid );
        path[0] = '\0';
        ++path_len;
    }

    memset( &addr, 0, sizeof( addr ) );
    addr.sun_family = AF_LOCAL;
    memset( addr.sun_path, 0, sizeof( addr.sun_path ) );
    memcpy( addr.sun_path, path, path_len );

    if ( connect ( fd, ( struct sockaddr* ) &addr, sizeof(addr) ) < 0 ) {
        LOGE ( "connect_helper: connect error, sock=%s", path);
        return -1;
    }

    if ( fd < 0 ) {
        LOGE( " connect_helper: socket error" );
        return -1;
    }

    return fd;
}

JNIEXPORT void JNICALL
Java_info_guardianproject_gpg_pinentry_PinentryDialog_connectToGpgAgent ( JNIEnv* env, jobject self, jint app_uid )
{
    LOGE("connectToGpgAgent");
    int in, out, sock;

    _ctx.env = env;
    pe_activity_init(&_ctx, self);
    pe_get_internal_gnupghome(&_ctx);

    sock = connect_helper( app_uid );
    if ( sock < 0 ) {
        LOGE("connectToGpgAgent aborting");
        return;
    }

    /*
     * we make sure we've connected to the correct server by checking that the
     * app_uid we passed (from our starting Intent) is the same uid of our peer.
     * This should always succeed, and doesn't provide any assurance we're NOT
     * connected to a malicious pinentry, but we check it because we can.
     * If it does fail, something incredibly janky is going on
     */
    struct ucred credentials;
    int ucred_length = sizeof( struct ucred );
    if ( getsockopt( sock, SOL_SOCKET, SO_PEERCRED, &credentials, &ucred_length ) ) {
        LOGE("connectToGpgAgent: couldn't obtain peer's credentials");
        close( sock );
        return;
    }

    if ( app_uid != credentials.uid ) {
        LOGE( "connectToGpgAgent: authentication error. Something JANKY is going on!" );
        LOGE( "                   expected uid %d, but found %d", app_uid, credentials.uid );
        close( sock );
        return;
    }

    /*
     * fetch the stdin and stdout from the helper
     * over the socket so that we can
     * directly communicate with gpg-agent
     */
    in = recv_fd ( sock );
    if ( in == -1 ) {
        LOGE ( "STDIN receiving failed!\n" );
    }
    out = recv_fd ( sock );
    if ( out == -1 ) {
        LOGE ( "STDOUT receiving failed!\n" );
    }

    /*
     * now we can act like a normal pinentry
     */
    LOGD("try pinentry_init()");
    pinentry_init ( "pinentry-android" );

    /* Consumes all arguments.  */
    LOGD("try pinentry_parse_opts()");
    if ( pinentry_parse_opts ( 0, 0 ) )
        write ( sock, EXIT_SUCCESS, 1 );

    // this only exits when done
    pinentry_loop2 ( in, out );
    LOGD("pinentry_loop2  returned");

    /*
     * the helper proces has stayed alive waiting for us
     * to finish, so here we send back the exit code
     */
    int buf[1] = { EXIT_SUCCESS };
    int r = write ( sock, buf, 1 );
    if ( r < 0 )
        LOGE ( "closing pinentry helper failed:" );
    close( sock );
}

static JNINativeMethod sMethods[] = {
    {"connectToGpgAgent", "(I)V", ( void* ) Java_info_guardianproject_gpg_pinentry_PinentryDialog_connectToGpgAgent}
};

JNIEXPORT jint JNICALL
JNI_OnLoad ( JavaVM* vm, void* reserved )
{
    // TODO set locale from the JavaVM's config
    // setlocale(LC_ALL, "");

    pe_context_init(&_ctx, vm);
    return JNI_VERSION_1_6;
}
