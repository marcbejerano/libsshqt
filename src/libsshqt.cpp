
#include <QDebug>
#include <QMetaEnum>
#include <QProcessEnvironment>
#include <QCoreApplication>
#include <QUrl>

#include "libsshqt.h"



//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// Misc
//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

#define LIBSSHQT_DEBUG(_message_) \
    if ( debug_output_ ) { \
        qDebug() << qPrintable(debug_prefix_) << _message_; \
    }

#define LIBSSHQT_CRITICAL(_message_) \
    qCritical() << qPrintable(debug_prefix_) << _message_;

#define LIBSSHQT_FATAL(_message_) \
    qFatal("%s Fatal error: %s", qPrintable(debug_prefix_), _message_);

#define LIBSSHQT_SET_OPT(_opt_, _val_, _log_val_) \
    if ( state_ != StateClosed ) { \
        LIBSSHQT_CRITICAL("Cannot set option" <<  #_opt_ << \
                          "to" << _log_val_ << \
                          "because current state is not StateClosed") \
    } \
    if ( debug_output_ ) { \
        LIBSSHQT_DEBUG("Setting option" << #_opt_ << "to" << _log_val_) \
    } \
    if ( ssh_options_set(session_, _opt_, _val_) != 0 ) { \
        LIBSSHQT_CRITICAL("Failed to set option " <<  #_opt_ << \
                          "to" << _log_val_)\
    }

static QString _getDebugPrefix(QObject *object)
{
    quint64 val    = reinterpret_cast<quint64>(object);
    QString hex    = QString("%1").arg(val, 0, 16);
    QString name   = object->metaObject()->className();
    QString prefix = hex.toUpper() + "-" + name + ":";

    return prefix;
}



//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// LibsshQtSession
//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

LibsshQtClient::LibsshQtClient(QObject *parent) :
    QObject(parent),
    debug_output_(false),
    session_(0),
    state_(StateClosed),
    process_state_running_(false),
    enable_writable_nofifier_(false),
    port_(22),
    read_notifier_(0),
    write_notifier_(0),
    unknown_host_type_(HostKnown)
{
    debug_prefix_ = _getDebugPrefix(this);

    if ( QProcessEnvironment::systemEnvironment().contains("LIBSSHQT_DEBUG")) {
        debug_output_ = true;
        LIBSSHQT_DEBUG("Constructor");
    }

    timer_.setSingleShot(true);
    timer_.setInterval(0);
    connect(&timer_, SIGNAL(timeout()), this, SLOT(processStateGuard()));

    session_ = ssh_new();
    if ( ! session_ ) {
        LIBSSHQT_FATAL("Could not create SSH session");
    }
    ssh_set_blocking(session_, 0);

    if (debug_output_) {
        setVerbosity(LogProtocol);
    } else {
        setVerbosity(LogDisable);
    }

    // Ensure that connections are always properly closed
    connect(qApp, SIGNAL(aboutToQuit()), this, SLOT(disconnectFromHost()));
}

LibsshQtClient::~LibsshQtClient()
{
    LIBSSHQT_DEBUG("Destructor");

    disconnectFromHost();
    if ( session_ ) {
        ssh_free(session_);
        session_ = 0;
    }
}

const char *LibsshQtClient::flagToString(const LogFlag flag)
{
    return staticMetaObject.enumerator(
                staticMetaObject.indexOfEnumerator("LogFlag"))
                    .valueToKey(flag);
}

const char *LibsshQtClient::flagToString(const StateFlag flag)
{
    return staticMetaObject.enumerator(
                staticMetaObject.indexOfEnumerator("StateFlag"))
                    .valueToKey(flag);
}

const char *LibsshQtClient::flagToString(const HostFlag flag)
{
    return staticMetaObject.enumerator(
                staticMetaObject.indexOfEnumerator("HostFlag"))
                    .valueToKey(flag);
}

const char *LibsshQtClient::flagToString(const AuthMehodFlag flag)
{
    return staticMetaObject.enumerator(
                staticMetaObject.indexOfEnumerator("AuthMehodFlag"))
                    .valueToKey(flag);
}

QString LibsshQtClient::flagsToString(const AuthMethods flags)
{
    QStringList list;

    int val = flags;
    if ( val == AuthMethodUnknown ) {
        list << flagToString(AuthMethodUnknown);

    } else {

        if ( flags.testFlag(AuthMethodNone)) {
            list << flagToString(AuthMethodNone);
        }

        if ( flags.testFlag(AuthMethodPassword)) {
            list << flagToString(AuthMethodPassword);
        }

        if ( flags.testFlag(AuthMethodPublicKey)) {
            list << flagToString(AuthMethodPublicKey);
        }

        if ( flags.testFlag(AuthMethodHostBased)) {
            list << flagToString(AuthMethodHostBased);

        }

        if ( flags.testFlag(AuthMethodInteractive)) {
            list << flagToString(AuthMethodInteractive);
        }
    }

    return QString("AuthMethods(%1)").arg(list.join(", "));
}

/*!
    Enable or disable debug messages.

    If logging is enabled libssh log level is set to LogProtocol and
    LIBSSHQT debug messages are enabled.
*/
void LibsshQtClient::setDebug(bool enabled)
{
    if ( enabled ) {
        debug_output_ = true;
        LIBSSHQT_DEBUG("Enabling debug messages");
        setVerbosity(LogProtocol);

    } else {
        LIBSSHQT_DEBUG("Disabling debug messages");
        debug_output_ = false;
        setVerbosity(LogDisable);
    }
}

void LibsshQtClient::setUsername(QString username)
{
    username_ = username;
    LIBSSHQT_SET_OPT(SSH_OPTIONS_USER, qPrintable(username), username)
}

void LibsshQtClient::setHostname(QString hostname)
{
    hostname_ = hostname;
    LIBSSHQT_SET_OPT(SSH_OPTIONS_HOST, qPrintable(hostname), hostname)
}

void LibsshQtClient::setPort(quint16 port)
{
    port_ = port;
    unsigned int tmp = port;
    LIBSSHQT_SET_OPT(SSH_OPTIONS_PORT, &tmp, port)
}

/*!
    Set libssh logging level.
*/
void LibsshQtClient::setVerbosity(LogFlag loglevel)
{
    int tmp = loglevel;
    LIBSSHQT_SET_OPT(SSH_OPTIONS_LOG_VERBOSITY, &tmp, loglevel);
}


bool LibsshQtClient::isDebugEnabled() const
{
    return debug_output_;
}

QString LibsshQtClient::username() const
{
    return username_;
}

QString LibsshQtClient::hostname() const
{
    return hostname_;
}

quint16 LibsshQtClient::port() const
{
    return port_;
}

QUrl LibsshQtClient::url() const
{
    QUrl url;
    url.setHost(hostname_);
    url.setPort(port_);
    url.setUserName(username_);
    url.setScheme("ssh");
    return url;
}

/*!
    Set hostname, port and username options from QUrl
*/
void LibsshQtClient::setUrl(QUrl &url)
{
    if ( url.scheme().toLower() == "ssh" ) {
        LIBSSHQT_DEBUG("Setting options from URL" << url);

        if ( url.port() > -1 ) {
            setPort(url.port());
        } else {
            setPort(22);
        }

        setHostname(url.host());
        setUsername(url.userName());

    } else {
        LIBSSHQT_CRITICAL("Not SSH URL:" << url);
    }
}

/*!
    Returns true if connection is successfully connected and authenticated.
*/
bool LibsshQtClient::isOpen()
{
    return state_ == StateOpened;
}

/*!
    Get supported authentication methods.
*/
LibsshQtClient::AuthMethods LibsshQtClient::supportedAuthMethods()
{
    return AuthMethods(ssh_userauth_list(session_, 0));
}

/*!
    Open connection to the host.
*/
void LibsshQtClient::connectToHost()
{
    if ( state_ == StateClosed ) {
        setState(StateConnecting);
        timer_.start();
    }
}

/*!
    Set hostname and open connection to the host.
*/
void LibsshQtClient::connectToHost(const QString hostName)
{
    if ( state_ == StateClosed ) {
        setHostname(hostName);
        setPort(22);
        connectToHost();
    }
}

/*!
    Set hostname and port and open connection to the host.
*/
void LibsshQtClient::connectToHost(const QString hostName, unsigned int port)
{
    if ( state_ == StateClosed ) {
        setHostname(hostName);
        setPort(port);
        connectToHost();
    }
}

/*!
    Close connection to host.
*/
void LibsshQtClient::disconnectFromHost()
{
    if ( state_ != StateClosed &&
         state_ != StateClosing ) {

        // Prevent recursion
        setState(StateClosing);

        // Child libsshqt objects must handle this and release all libssh
        // resources
        emit doCleanup();

        if ( read_notifier_ ) {
            read_notifier_->setEnabled(false);
            read_notifier_->deleteLater();
            read_notifier_ = 0;
        }

        if ( write_notifier_ ) {
            write_notifier_->setEnabled(false);
            write_notifier_->deleteLater();
            write_notifier_ = 0;
        }

        ssh_disconnect(session_);
        setState(StateClosed);
    }
}

/*!
    Enable or disable the use of 'None' SSH authentication.
*/
void LibsshQtClient::useNoneAuth(bool enabled)
{
    enableDisableAuth(enabled, UseAuthNone);
}

/*!
    Enable or disable the use of automatic public key authentication.

    This includes keys stored in ssh-agent and in ~/.ssh/ directory.
*/
void LibsshQtClient::useAutoKeyAuth(bool enabled)
{
    enableDisableAuth(enabled, UseAuthAutoPubKey);
}

/*!
    Enable or disable the use of password based SSH authentication.
*/
void LibsshQtClient::usePasswordAuth(bool enabled, QString password)
{
    enableDisableAuth(enabled, UseAuthPassword);
    password_ = password;
}

/*!
    Enable or disable the use of Interactive SSH authentication.
*/
void LibsshQtClient::useInteractiveAuth(bool enabled)
{
    enableDisableAuth(enabled, UseAuthInteractive);
}

LibsshQtClient::UseAuths LibsshQtClient::failedAuths()
{
    return failed_auths_;
}

/*!
    Run a command
*/
LibsshQtProcess *LibsshQtClient::runCommand(QString command)
{
    LibsshQtProcess *process = new LibsshQtProcess(this);
    process->setCommand(command);
    process->openChannel();
    return process;
}

LibsshQtClient::HostFlag LibsshQtClient::unknownHostType()
{
    return unknown_host_type_;
}

/*!
    Get a message to show to the user why the host is unknown.
*/
QString LibsshQtClient::unknownHostMessage()
{
    switch ( unknown_host_type_ ) {

    case HostKnown:
        return QString() +
                "Server is known. Public key used by this server is:" +
                hostPublicKeyHash();

    case HostUnknown:
    case HostKnownHostsFileMissing:
        return QString() +
                "The server is unknown. Do you want to add the server to the" +
                "known servers? Public key used by this server is:" +
                hostPublicKeyHash();

    case HostKeyChanged:
        return QString() +
                "WARNING: Public key sent by the server does not match" +
                "expected value. A third party may be attempting to " +
                "impersonate the server. Public key used by this server is:" +
                hostPublicKeyHash();

    case HostKeyTypeChanged:
        return QString() +
                "WARNING: Public key type sent by the server does not match" +
                "expected value. A third party may be attempting to " +
                "impersonate the server. Public key used by this server is:" +
                hostPublicKeyHash();

    }

    Q_ASSERT_X(false, __func__, "Case was not handled properly");
    return QString();
}

/*!
    Get MD5 hexadecimal hash of the servers public key.
*/
QString LibsshQtClient::hostPublicKeyHash()
{
    QString string;
    int hash_len = 0;
    unsigned char *hash = NULL;
    char *hexa = NULL;

    hash_len = ssh_get_pubkey_hash(session_, &hash);
    hexa = ssh_get_hexa(hash, hash_len);

    string = QString(hexa);

    free(hexa);
    free(hash);

    return string;
}

/*!
    Add current host to the known hosts file.
*/
bool LibsshQtClient::markCurrentHostKnown()
{
    int rc = ssh_write_knownhost(session_);

    switch ( rc ) {
    case SSH_OK:
        return true;

    case SSH_ERROR:
        LIBSSHQT_DEBUG("Could not add current host to known host list");
        return false;

    default:
        LIBSSHQT_CRITICAL("Unknown result code" << rc <<
                          "received from ssh_write_knownhost()");
        return false;
    }
}

/*!
    Get error code and message from libssh.
*/
QString LibsshQtClient::errorCodeAndMessage() const
{
    return QString("%1: %2").arg(errorCode()).arg(errorMessage());
}

/*!
    Get error message from libssh.
*/
QString LibsshQtClient::errorMessage() const
{
    return QString(ssh_get_error(session_));
}

/*!
    Get error code from libssh.
*/
int LibsshQtClient::errorCode() const
{
    return ssh_get_error_code(session_);
}

LibsshQtClient::StateFlag LibsshQtClient::state() const
{
    return state_;
}

ssh_session LibsshQtClient::sshSession()
{
    return session_;
}

void LibsshQtClient::enableWritableNotifier()
{
    if ( process_state_running_ ) {
        enable_writable_nofifier_ = true;
    } else if ( write_notifier_ ) {
        write_notifier_->setEnabled(true);
    }
}

/*!
    Change session state and send appropriate signals.
*/
void LibsshQtClient::setState(StateFlag state)
{
    if ( state_ == state ) {
        LIBSSHQT_DEBUG("State is already" << state);
        return;
    }

    LIBSSHQT_DEBUG("Changing state to" << state);
    state_ = state;

    switch ( state_ ) {
    case StateClosed:           emit closed();          break;
    case StateClosing:                                  break;
    case StateConnecting:                               break;
    case StateIsKnown:                                  break;
    case StateUnknownHost:      emit unknownHost();     break;
    case StateAuthChoose:       emit chooseAuth();      break;
    case StateAuthContinue:                             break;
    case StateAuthNone:                                 break;
    case StateAuthAutoPubkey:                           break;
    case StateAuthPassword:                             break;
    case StateAuthInteractive:                          break;
    case StateAuthFailed:       emit authFailed();      break;
    case StateOpened:           emit opened();          break;
    case StateError:            emit error();           break;
    }
}

/*!
    Choose next authentication method to try.

    if authentication methods have not been chosen or all chosen authentication
    methods have failed, switch state to StateChooseAuth or StateAuthFailed,
    respectively.
*/
void LibsshQtClient::tryNextAuth()
{
    // Detect failed authentication methods
    switch ( state_ ) {
    case StateClosed:
    case StateClosing:
    case StateConnecting:
    case StateIsKnown:
    case StateUnknownHost:
    case StateAuthChoose:
    case StateAuthContinue:
    case StateAuthFailed:
    case StateOpened:
    case StateError:
        break;

    case StateAuthNone:
        failed_auths_ |= UseAuthNone;
        break;

    case StateAuthAutoPubkey:
        failed_auths_ |= UseAuthAutoPubKey;
        break;

    case StateAuthPassword:
        failed_auths_ |= UseAuthPassword;
        break;

    case StateAuthInteractive:
        failed_auths_ |= UseAuthInteractive;
        break;
    }

    // Choose next state for LibsshQtClient
    if ( use_auths_ == UseAuthEmpty && failed_auths_ == UseAuthEmpty ) {
        setState(StateAuthChoose);

    } else if ( use_auths_ == UseAuthEmpty ) {
        setState(StateAuthFailed);

    } else if ( use_auths_ & UseAuthNone ) {
        use_auths_ &= ~UseAuthNone;
        setState(StateAuthNone);
        timer_.start();

    } else if ( use_auths_ & UseAuthAutoPubKey ) {
        use_auths_ &= ~UseAuthAutoPubKey;
        setState(StateAuthAutoPubkey);
        timer_.start();

    } else if ( use_auths_ & UseAuthPassword ) {
        use_auths_ &= ~UseAuthPassword;
        setState(StateAuthPassword);
        timer_.start();

    } else if ( use_auths_ & UseAuthInteractive ) {
        use_auths_ &= ~UseAuthInteractive;
        setState(StateAuthInteractive);
        timer_.start();
    }
}

void LibsshQtClient::setUpNotifiers()
{
    if ( ! read_notifier_ ) {
        socket_t socket = ssh_get_fd(session_);

        LIBSSHQT_DEBUG("Setting up read notifier for socket" << socket);
        read_notifier_ = new QSocketNotifier(socket,
                                             QSocketNotifier::Read,
                                             this);
        read_notifier_->setEnabled(true);
        connect(read_notifier_, SIGNAL(activated(int)),
                this, SLOT(handleSocketReadable(int)));
    }

    if ( ! write_notifier_ ) {
        socket_t socket = ssh_get_fd(session_);

        LIBSSHQT_DEBUG("Setting up write notifier for socket" << socket);
        write_notifier_ = new QSocketNotifier(socket,
                                              QSocketNotifier::Write,
                                              this);
        write_notifier_->setEnabled(true);
        connect(write_notifier_, SIGNAL(activated(int)),
                this, SLOT(handleSocketWritable(int)));
    }
}

void LibsshQtClient::handleSocketReadable(int socket)
{
    Q_UNUSED( socket );

    read_notifier_->setEnabled(false);
    processStateGuard();
    read_notifier_->setEnabled(true);
}

void LibsshQtClient::handleSocketWritable(int socket)
{
    Q_UNUSED( socket );

    enable_writable_nofifier_ = false;
    write_notifier_->setEnabled(false);
    processStateGuard();
}

void LibsshQtClient::processStateGuard()
{
    Q_ASSERT( ! process_state_running_ );
    if ( process_state_running_ ) return;

    process_state_running_ = true;
    processState();
    process_state_running_ = false;

    if ( enable_writable_nofifier_ ) {
        write_notifier_->setEnabled(true);
    }
}

void LibsshQtClient::processState()
{
    switch ( state_ ) {

    case StateClosed:
    case StateClosing:
    case StateUnknownHost:
    case StateAuthChoose:
    case StateAuthFailed:
    case StateError:
        return;

    case StateConnecting:
    {
        int rc = ssh_connect(session_);
        if ( rc != SSH_ERROR &&
             ( read_notifier_ == 0 || write_notifier_ == 0 )) {
            setUpNotifiers();
        }

        switch ( rc ) {
        case SSH_AGAIN:
            enableWritableNotifier();
            return;

        case SSH_ERROR:
            LIBSSHQT_DEBUG("Channel open error:" << errorCodeAndMessage());
            setState(StateError);
            return;

        case SSH_OK:
            setState(StateIsKnown);
            timer_.start();
            return;

        default:
            LIBSSHQT_CRITICAL("Unknown result code" << rc <<
                              "received from ssh_connect()");
            return;
        }
    } break;

    case StateIsKnown:
    {
        int known = ssh_is_server_known(session_);

        switch ( known ) {
        case SSH_SERVER_ERROR:
            setState(StateError);
            return;

        case SSH_SERVER_NOT_KNOWN:
        case SSH_SERVER_KNOWN_CHANGED:
        case SSH_SERVER_FOUND_OTHER:
        case SSH_SERVER_FILE_NOT_FOUND:
            unknown_host_type_ = static_cast< HostFlag >( known );
            LIBSSHQT_DEBUG("Setting unknown host state to" <<
                           unknown_host_type_);
            setState(StateUnknownHost);
            return;

        case SSH_SERVER_KNOWN_OK:
            unknown_host_type_ = HostKnown;
            tryNextAuth();
            return;

        default:
            LIBSSHQT_CRITICAL("Unknown result code" << known <<
                              "received from ssh_is_server_known()");
            return;
        }
    } break;

    case StateAuthContinue: {
        tryNextAuth();
        return;
    } break;

    case StateAuthNone:
    {
        int rc = ssh_userauth_none(session_, 0);
        handleAuthResponse(rc, "ssh_userauth_none", UseAuthNone);
        return;
    } break;

    case StateAuthAutoPubkey:
    {
        int rc = ssh_userauth_autopubkey(session_, 0);
        handleAuthResponse(rc, "ssh_userauth_autopubkey", UseAuthAutoPubKey);
        return;
    } break;

    case StateAuthPassword:
    {
        QByteArray utf8pw = password_.toUtf8();
        int rc = ssh_userauth_password(session_, 0, utf8pw.constData());
        handleAuthResponse(rc, "ssh_userauth_password", UseAuthPassword);
        return;
    } break;

    case StateAuthInteractive: {
        //! @TODO
        return;
    }

    case StateOpened:
    {
        // Activate processState() function on all children so that they can
        // process their events and read and write IO.
        emit doProcessState();
        return;
    } break;

    } // End switch

    Q_ASSERT_X(false, __func__, "Case was not handled properly");
}

void LibsshQtClient::enableDisableAuth(bool enabled, UseAuthFlag auth)
{
    if ( enabled ) {
        use_auths_ |= auth;
        if ( state_ == StateAuthChoose || state_ == StateAuthFailed ) {
            setState(StateAuthContinue);
            timer_.start();
        }

    } else {
        use_auths_ &= ~auth;
    }
}

void LibsshQtClient::handleAuthResponse(int         rc,
                                        const char *func,
                                        UseAuthFlag auth)
{
    switch ( rc ) {
    case SSH_AUTH_AGAIN:
        enableWritableNotifier();
        return;

    case SSH_AUTH_ERROR:
        LIBSSHQT_DEBUG("Authentication error:" << auth <<
                       errorCodeAndMessage());
        setState(StateError);
        return;

    case SSH_AUTH_DENIED:
        LIBSSHQT_DEBUG("Authentication denied:" << auth);
        tryNextAuth();
        return;

    case SSH_AUTH_PARTIAL:
        LIBSSHQT_DEBUG("Partial authentication:" << auth);
        tryNextAuth();
        return;

    case SSH_AUTH_SUCCESS:
        LIBSSHQT_DEBUG("Authentication success:" << auth);
        succeeded_auth_ = auth;
        setState(StateOpened);
        timer_.start();
        return;

    default:
        LIBSSHQT_CRITICAL("Unknown result code" << rc <<
                          "received from" << func);
        return;
    }

    Q_ASSERT_X(false, __func__, "Case was not handled properly");
}




//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// LibsshQtChannel
//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

LibsshQtChannel::LibsshQtChannel(LibsshQtClient *parent) :
    QIODevice(parent),
    client_(parent),
    debug_output_(false),
    channel_(0),
    eof_state_(EofNotSent),
    buffer_size_(1024 * 16),
    write_size_(1024 * 16)
{
    debug_prefix_ = _getDebugPrefix(this);
    if ( client_->isDebugEnabled()) {
        debug_output_ = true;
    }
}

LibsshQtChannel::~LibsshQtChannel()
{
    close();
    eof_state_ = EofNotSent;
}

/*!
    Get parent LibsshQtClient object.
*/
LibsshQtClient *LibsshQtChannel::client()
{
    return client_;
}

const char *LibsshQtChannel::flagToString(const EofState flag)
{
    return staticMetaObject.enumerator(
                staticMetaObject.indexOfEnumerator("EofState"))
                    .valueToKey(flag);
}

/*!
    Set maximum amount of data that LibsshQtChannel will write to the channel
    in one go.
*/
void LibsshQtChannel::setWriteSize(int write_size)
{
    static const int min_size = 4096;
    Q_ASSERT( write_size >= min_size );

    if ( write_size_ >= min_size ) {
        write_size_ = write_size;
    } else {
        write_size_ = min_size;
    }
}

/*!
    Set read buffer size.
*/
void LibsshQtChannel::setReadBufferSize(int buffer_size)
{
    static const int min_size = 4096;
    Q_ASSERT( buffer_size >= min_size );

    if ( buffer_size >= min_size ) {
        buffer_size_ = buffer_size;
    } else {
        buffer_size_ = min_size;
    }
}

int LibsshQtChannel::getBufferSize()
{
    return buffer_size_;
}

int LibsshQtChannel::getWriteSize()
{
    return write_size_;
}

/*!
    Send EOF to the channel once write buffer has been written to the channel.
 */
void LibsshQtChannel::sendEof()
{
    if ( eof_state_ == EofNotSent ) {
        LIBSSHQT_DEBUG("EOF queued");
        eof_state_ = EofQueued;
    }
}

LibsshQtChannel::EofState LibsshQtChannel::eofState()
{
    return eof_state_;
}

/*!
  Get error code and message from libssh.
*/
QString LibsshQtChannel::errorCodeAndMessage() const
{
    return QString("%1, %2").arg(errorCode()).arg(errorMessage());
}

/*!
  Get error message from libssh.
*/
QString LibsshQtChannel::errorMessage() const
{
    if ( client_->state() == LibsshQtClient::StateError ) {
        return client_->errorMessage();
    } else if ( channel_ ) {
        return QString(ssh_get_error(channel_));
    } else {
        return QString();
    }
}

/*!
  Get error code from libssh.
*/
int LibsshQtChannel::errorCode() const
{
    if ( client_->state() == LibsshQtClient::StateError ) {
        return client_->errorCode();
    } else if ( channel_ ) {
        return ssh_get_error_code(channel_);
    } else {
        return 0;
    }
}

qint64 LibsshQtChannel::bytesAvailable() const
{
    return read_buffer_.size() + QIODevice::bytesAvailable();
}

qint64 LibsshQtChannel::bytesToWrite() const
{
    return write_buffer_.size();
}

bool LibsshQtChannel::isSequential()
{
    return true;
}

bool LibsshQtChannel::canReadLine() const
{
    return QIODevice::canReadLine() ||
           read_buffer_.contains('\n') ||
           read_buffer_.size() >= buffer_size_ ||
           ( isOpen() == false && atEnd() == false );
}

/*!
  Set QIODevice to open state.
*/
bool LibsshQtChannel::open(OpenMode mode)
{
    Q_ASSERT( mode == QIODevice::ReadWrite );
    Q_UNUSED( mode );

    // Set Unbuffered to disable QIODevice buffers.
    bool ret = QIODevice::open(QIODevice::ReadWrite | QIODevice::Unbuffered);
    Q_ASSERT(ret);
    return ret;
}

void LibsshQtChannel::close()
{
    if ( channel_ ) {
        if ( ssh_channel_is_open(channel_) != 0 ) {
            ssh_channel_close(channel_);
        }

        ssh_channel_free(channel_);
        channel_ = 0;
        QIODevice::close();
    }
}

void LibsshQtChannel::checkIo()
{
    if ( ! channel_ ) return;

    bool emit_ready_read = false;
    bool emit_bytes_written = false;

    int read_size = 0;
    int read_available = ssh_channel_poll(channel_, false);
    if ( read_available > 0 ) {

        // Dont read more than buffer_size_ specifies.
        int max_read = buffer_size_ - read_buffer_.size();
        if ( read_available > max_read ) {
            read_available = max_read;
        }

        if ( read_available > 0 ) {
            char data[read_available + 1];
            data[read_available] = 0;

            read_size = ssh_channel_read_nonblocking(channel_, data,
                                                     read_available, false);
            Q_ASSERT(read_size >= 0);

            read_buffer_.reserve(buffer_size_);
            read_buffer_.append(data, read_size);

            LIBSSHQT_DEBUG("Read:" << read_size <<
                           " Data in buffer:" << read_buffer_.size() <<
                           " Readable from channel:" <<
                           ssh_channel_poll(channel_, false));
            if ( read_size > 0 ) {
                emit_ready_read = true;
            }
        }
    }

    int written = 0;
    int writable = write_buffer_.size();
    if ( writable > write_size_ ) {
        writable = write_size_;
    }

    if ( writable > 0 ) {
        written = ssh_channel_write(channel_,
                                    write_buffer_.constData(),
                                    writable);
        Q_ASSERT(written >= 0);
        write_buffer_.remove(0, written);

        LIBSSHQT_DEBUG("Wrote" << written << "bytes to channel")
        if ( written > 0 ) {
            emit_bytes_written = true;
        }
    }

    // Write more data once the socket is ready
    if ( write_buffer_.size() > 0 ) {
        client_->enableWritableNotifier();
    }

    // Send EOF once all data has been written to channel
    if ( eof_state_ == EofQueued && write_buffer_.size() == 0 ) {
        LIBSSHQT_DEBUG("Sending EOF to channel");
        ssh_channel_send_eof(channel_);
        eof_state_ = EofSent;
    }

    // Emit signals here, so that somebody wont call closeChannel() while
    // when we are reading from it.
    if ( emit_ready_read ) {
        emit readyRead();
    }
    if ( emit_bytes_written ) {
        emit bytesWritten(written);
    }
}

qint64 LibsshQtChannel::readData(char *data, qint64 maxlen)
{
    queueCheckIo();

    qint64 copy_len  = maxlen;
    int    data_size = read_buffer_.size();

    if ( copy_len > data_size ) {
        copy_len = data_size;
    }

    memcpy(data, read_buffer_.constData(), copy_len);
    read_buffer_.remove(0, copy_len);
    return copy_len;
}

qint64 LibsshQtChannel::writeData(const char *data, qint64 len)
{
    if ( eof_state_ == EofNotSent ) {
        client_->enableWritableNotifier();
        write_buffer_.reserve(write_size_);
        write_buffer_.append(data, len);
        return len;

    } else {
        LIBSSHQT_CRITICAL("Cannot write to channel because EOF state is" <<
                          eof_state_);
        return 0;
    }
}



//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// LibsshQtProcess
//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

LibsshQtProcess::LibsshQtProcess(LibsshQtClient *parent) :
    LibsshQtChannel(parent),
    state_(StateClosed),
    exit_code_(-1),
    stderr_(new LibsshQtProcessStderr(this))
{
    debug_prefix_ = _getDebugPrefix(this);
    stderr_->err_buffer_ = &stderr_buffer_;

    LIBSSHQT_DEBUG("Constructor");

    timer_.setSingleShot(true);
    timer_.setInterval(0);

    connect(&timer_, SIGNAL(timeout()),        this, SLOT(processState()));
    connect(parent,  SIGNAL(error()),          this, SLOT(handleClientError()));
    connect(parent,  SIGNAL(doProcessState()), this, SLOT(processState()));
    connect(parent,  SIGNAL(doCleanup()),      this, SLOT(closeChannel()));

    setStdoutBehaviour(OutputToQDebug, "Remote stdout:");
    setStderrBehaviour(OutputToQDebug, "Remote stderr:");
}

LibsshQtProcess::~LibsshQtProcess()
{
    LIBSSHQT_DEBUG("Destructor");
    closeChannel();
}

const char *LibsshQtProcess::flagToString(const StateFlag flag)
{
    return staticMetaObject.enumerator(
                staticMetaObject.indexOfEnumerator("StateFlag"))
                    .valueToKey(flag);
}

const char *LibsshQtProcess::flagToString(const OutputBehaviourFlag flag)
{
    return staticMetaObject.enumerator(
                staticMetaObject.indexOfEnumerator("OutputBehaviourFlag"))
                    .valueToKey(flag);
}

void LibsshQtProcess::setCommand(QString command)
{
    command_ = command;
    LIBSSHQT_DEBUG("Setting command to" << command);
}

int LibsshQtProcess::exitCode() const
{
    return exit_code_;
}

void LibsshQtProcess::setStdoutBehaviour(OutputBehaviourFlag behaviour,
                                     QString             prefix)
{
    LIBSSHQT_DEBUG("Setting stdout behaviour to" << behaviour);

    stdout_behaviour_     = behaviour;
    stdout_output_prefix_ = prefix;

    if ( behaviour == OutputManual ) {
        disconnect(this, SIGNAL(readyRead()),
                   this, SLOT(handleStdoutOutput()));

    } else {
        connect(this, SIGNAL(readyRead()),
                this, SLOT(handleStdoutOutput()),
                Qt::UniqueConnection);
        handleStdoutOutput();
    }
}

void LibsshQtProcess::setStderrBehaviour(OutputBehaviourFlag behaviour,
                                     QString             prefix)
{
    LIBSSHQT_DEBUG("Setting stderr behaviour to" << behaviour);

    stderr_behaviour_     = behaviour;
    stderr_output_prefix_ = prefix;

    if ( behaviour == OutputManual ) {
        disconnect(stderr_, SIGNAL(readyRead()),
                   this, SLOT(handleStderrOutput()));

    } else {
        connect(stderr_, SIGNAL(readyRead()),
                this, SLOT(handleStderrOutput()),
                Qt::UniqueConnection);
        handleStderrOutput();
    }
}

LibsshQtProcessStderr *LibsshQtProcess::stderr()
{
    return stderr_;
}

void LibsshQtProcess::openChannel()
{
    if ( state_ == StateClosed ) {
        setState(StateWaitClient);
        timer_.start();
    }
}

void LibsshQtProcess::closeChannel()
{
    if ( state_ != StateClosed &&
         state_ != StateClosing ){

        // Prevent recursion
        setState(StateClosing);

        emit readChannelFinished();
        handleStdoutOutput();
        handleStderrOutput();

        close();
        stderr_->close();
        setState(StateClosed);
    }
}

LibsshQtProcess::StateFlag LibsshQtProcess::state() const
{
    return state_;
}

void LibsshQtProcess::setState(StateFlag state)
{
    if ( state_ == state ) {
        LIBSSHQT_DEBUG("State is already" << state);
        return;
    }

    LIBSSHQT_DEBUG("Changing state to" << state);
    state_ = state;

    switch (  state_ ) {
    case StateClosed:       emit closed();          break;
    case StateClosing:                              break;
    case StateWaitClient:                           break;
    case StateOpening:                              break;
    case StateExec:                                 break;
    case StateOpen:         emit opened();          break;
    case StateError:        emit error();           break;
    case StateClientError:  emit error();           break;
    }
}

void LibsshQtProcess::processState()
{
    switch ( state_ ) {

    case StateClosed:
    case StateClosing:
    case StateError:
    case StateClientError:
        return;

    case StateWaitClient:
    {
        if ( client_->state() == LibsshQtClient::StateOpened ) {
            setState(StateOpening);
            timer_.start();
        }
        return;
    } break;

    case StateOpening:
    {
        if ( ! channel_ ) {
            channel_ = ssh_channel_new(client_->sshSession());
            if ( ! channel_ ) {
                LIBSSHQT_FATAL("Could not create SSH channel");
            }
        }

        int rc = ssh_channel_open_session(channel_);

        switch ( rc ) {
        case SSH_AGAIN:
            client_->enableWritableNotifier();
            return;

        case SSH_ERROR:
            LIBSSHQT_DEBUG("Channel open error:" << errorCodeAndMessage());
            setState(StateError);
            return;

        case SSH_OK:
            setState(StateExec);
            timer_.start();
            return;

        default:
            LIBSSHQT_CRITICAL("Unknown result code" << rc <<
                              "received from ssh_channel_open_session()");
            return;
        }
    } break;

    case StateExec:
    {
        int rc = ssh_channel_request_exec(channel_, qPrintable(command_));

        switch ( rc ) {
        case SSH_AGAIN:
            client_->enableWritableNotifier();
            return;

        case SSH_ERROR:
            LIBSSHQT_DEBUG("Channel exec error:" << errorCodeAndMessage());
            setState(StateError);
            return;

        case SSH_OK:
            LibsshQtChannel::open();
            stderr_->open();
            setState(StateOpen);
            timer_.start();
            return;

        default:
            LIBSSHQT_CRITICAL("Unknown result code" << rc <<
                              "received from ssh_channel_request_exec()");
            return;
        }
    } break;

    case StateOpen:
    {
        checkIo();

        if ( state_ == StateOpen &&
             ssh_channel_poll(channel_, false) == SSH_EOF &&
             ssh_channel_poll(channel_, true)  == SSH_EOF ) {

            exit_code_ = ssh_channel_get_exit_status(channel_);

            LIBSSHQT_DEBUG("Process channel EOF");
            LIBSSHQT_DEBUG("Command exit code:"     << exit_code_);
            LIBSSHQT_DEBUG("Data in read buffer:"   << read_buffer_.size());
            LIBSSHQT_DEBUG("Data in write buffer:"  << write_buffer_.size());
            LIBSSHQT_DEBUG("Data in stderr buffer:" << stderr_buffer_.size());

            closeChannel();
            emit finished(exit_code_);
            return;
        }

        return;
    } break;

    } // End switch

    Q_ASSERT_X(false, __func__, "Case was not handled properly");
}

void LibsshQtProcess::checkIo()
{
    LibsshQtChannel::checkIo();
    if ( ! channel_ ) return;

    // Read stderr data from channel
    int read = 0;
    int read_size = ssh_channel_poll(channel_, true);
    if ( read_size > 0 ) {

        // Dont read more than buffer_size_ specifies.
        int max_read = buffer_size_ - stderr_buffer_.size();
        if ( read_size > max_read ) {
            read_size = max_read;
        }

        if ( read_size > 0 ) {
            char data[read_size];
            read = ssh_channel_read_nonblocking(channel_, data, read_size, true);


            stderr_buffer_.reserve(buffer_size_);
            stderr_buffer_.append(data, read_size);

            LIBSSHQT_DEBUG("stderr: Read:" << read <<
                           " Data in buffer:" << stderr_buffer_.size() <<
                           " Readable from channel:" <<
                           ssh_channel_poll(channel_, true));

            if ( read > 0 ) {
                emit stderr_->readyRead();
            }
        }
    }
}

void LibsshQtProcess::queueCheckIo()
{
    timer_.start();
}

void LibsshQtProcess::handleClientError()
{
    setState(LibsshQtProcess::StateClientError);
}

void LibsshQtProcess::handleStdoutOutput()
{
    if ( stdout_behaviour_ == OutputManual) return;

    while ( canReadLine()) {
        QString line = QString(readLine());
        handleOutput(stdout_behaviour_, stdout_output_prefix_, line);
    }
}

void LibsshQtProcess::handleStderrOutput()
{
    if ( stderr_behaviour_ == OutputManual) return;

    Q_ASSERT(stderr_);
    while ( stderr_->canReadLine()) {
        QString line = QString(stderr()->readLine());
        handleOutput(stderr_behaviour_, stderr_output_prefix_, line);
    }
}

void LibsshQtProcess::handleOutput(OutputBehaviourFlag &behaviour,
                                   QString             &prefix,
                                   QString             &line)
{
    switch ( behaviour ) {
    case OutputManual:
    case OutputToDevNull:
        // Do nothing
        return;

    case OutputToQDebug:
        if ( line.endsWith('\n')) {
            line.remove(line.length() - 1, 1);
        }

        if ( prefix.isEmpty()) {
            qDebug() << qPrintable(line);
        } else {
            qDebug() << qPrintable(prefix)
                     << qPrintable(line);
        }
        return;
    }
}



//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
// LibsshQtProcessStderr
//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-

LibsshQtProcessStderr::LibsshQtProcessStderr(LibsshQtProcess *parent) :
    QIODevice(parent),
    parent_(parent),
    err_buffer_(0)
{
}

qint64 LibsshQtProcessStderr::bytesAvailable() const
{
    return err_buffer_->size() + QIODevice::bytesAvailable();
}

qint64 LibsshQtProcessStderr::bytesToWrite() const
{
    return 0;
}

bool LibsshQtProcessStderr::isSequential()
{
    return true;
}

bool LibsshQtProcessStderr::canReadLine() const
{
    return QIODevice::canReadLine() ||
           err_buffer_->contains('\n') ||
           err_buffer_->size() >= parent_->buffer_size_ ||
           ( isOpen() == false && atEnd() == false );
}

bool LibsshQtProcessStderr::open(OpenMode mode)
{
    Q_ASSERT( mode == QIODevice::ReadOnly );
    Q_UNUSED( mode );

    // Set Unbuffered to disable QIODevice buffers.
    bool ret = QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    Q_ASSERT(ret);
    return ret;
}

void LibsshQtProcessStderr::close()
{
    QIODevice::close();
}

qint64 LibsshQtProcessStderr::readData(char *data, qint64 maxlen)
{
    Q_ASSERT(err_buffer_);
    parent_->queueCheckIo();

    qint64 copy_len  = maxlen;
    int    data_size = err_buffer_->size();

    if ( copy_len > data_size ) {
        copy_len = data_size;
    }

    memcpy(data, err_buffer_->constData(), copy_len);
    err_buffer_->remove(0, copy_len);
    return copy_len;
}

qint64 LibsshQtProcessStderr::writeData(const char */*data*/, qint64 /*len*/)
{
    return 0;
}



















