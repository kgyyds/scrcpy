package com.genymobile.scrcpy.device;

import com.genymobile.scrcpy.control.ControlChannel;
import com.genymobile.scrcpy.util.IO;
import com.genymobile.scrcpy.util.StringUtils;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;

import java.io.Closeable;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

public final class DesktopConnection implements Closeable {

    private static final int DEVICE_NAME_FIELD_LENGTH = 64;

    private static final String SOCKET_NAME_PREFIX = "scrcpy";

    private final LocalSocket localVideoSocket;
    private final LocalSocket localAudioSocket;
    private final LocalSocket localControlSocket;

    private final Socket videoSocket;
    private final Socket audioSocket;
    private final Socket controlSocket;

    private final FileDescriptor videoFd;
    private final FileDescriptor audioFd;

    private final ControlChannel controlChannel;

    private DesktopConnection(LocalSocket localVideoSocket, LocalSocket localAudioSocket, LocalSocket localControlSocket,
            Socket videoSocket, Socket audioSocket, Socket controlSocket) throws IOException {
        this.localVideoSocket = localVideoSocket;
        this.localAudioSocket = localAudioSocket;
        this.localControlSocket = localControlSocket;
        this.videoSocket = videoSocket;
        this.audioSocket = audioSocket;
        this.controlSocket = controlSocket;

        if (localVideoSocket != null) {
            videoFd = localVideoSocket.getFileDescriptor();
        } else if (videoSocket != null) {
            // java.net.Socket does not expose a FileDescriptor directly
            videoFd = null; // Will need to handle this in getVideoFd()
        } else {
            videoFd = null;
        }

        if (localAudioSocket != null) {
            audioFd = localAudioSocket.getFileDescriptor();
        } else if (audioSocket != null) {
            // java.net.Socket does not expose a FileDescriptor directly
            audioFd = null; // Will need to handle this in getAudioFd()
        } else {
            audioFd = null;
        }

        if (localControlSocket != null) {
            controlChannel = new ControlChannel(localControlSocket);
        } else if (controlSocket != null) {
            // ControlChannel needs to be adapted for java.net.Socket
            controlChannel = new ControlChannel(controlSocket); // This will require a new ControlChannel constructor
        } else {
            controlChannel = null;
        }
    }



    private static String getSocketName(int scid) {
        if (scid == -1) {
            // If no SCID is set, use "scrcpy" to simplify using scrcpy-server alone
            return SOCKET_NAME_PREFIX;
        }

        return SOCKET_NAME_PREFIX + String.format("_%08x", scid);
    }

    private static LocalSocket connect(String abstractName) throws IOException {
        LocalSocket localSocket = new LocalSocket();
        localSocket.connect(new LocalSocketAddress(abstractName));
        return localSocket;
    }

    public static DesktopConnection open(int scid, boolean tunnelForward, boolean video, boolean audio, boolean control, boolean sendDummyByte,
            int listenVideoPort, int listenControlPort) throws IOException {
        String socketName = getSocketName(scid);

        LocalSocket videoLocalSocket = null;
        LocalSocket audioLocalSocket = null;
        LocalSocket controlLocalSocket = null;

        Socket videoSocket = null;
        Socket audioSocket = null;
        Socket controlSocket = null;

        try {
            if (listenVideoPort > 0 || listenControlPort > 0) {
                // Public network connection
                if (video) {
                    ServerSocket videoServerSocket = new ServerSocket(listenVideoPort);
                    videoSocket = videoServerSocket.accept();
                    if (sendDummyByte) {
                        videoSocket.getOutputStream().write(0);
                        sendDummyByte = false;
                    }
                }
                if (audio) {
                    // Assuming audio will use the same mechanism as video for simplicity, or a separate port if needed
                    // For now, let's assume it's part of the video connection or handled similarly
                    // If a separate audio port is required, it should be added to options and handled here
                }
                if (control) {
                    ServerSocket controlServerSocket = new ServerSocket(listenControlPort);
                    controlSocket = controlServerSocket.accept();
                    if (sendDummyByte) {
                        controlSocket.getOutputStream().write(0);
                        sendDummyByte = false;
                    }
                }
            } else {
                // Local (ADB) connection
                if (tunnelForward) {
                    try (LocalServerSocket localServerSocket = new LocalServerSocket(socketName)) {
                        if (video) {
                            videoLocalSocket = localServerSocket.accept();
                            if (sendDummyByte) {
                                // send one byte so the client may read() to detect a connection error
                                videoLocalSocket.getOutputStream().write(0);
                                sendDummyByte = false;
                            }
                        }
                        if (audio) {
                            audioLocalSocket = localServerSocket.accept();
                            if (sendDummyByte) {
                                // send one byte so the client may read() to detect a connection error
                                audioLocalSocket.getOutputStream().write(0);
                                sendDummyByte = false;
                            }
                        }
                        if (control) {
                            controlLocalSocket = localServerSocket.accept();
                            if (sendDummyByte) {
                                // send one byte so the client may read() to detect a connection error
                                controlLocalSocket.getOutputStream().write(0);
                                sendDummyByte = false;
                            }
                        }
                    }
                } else {
                    if (video) {
                        videoLocalSocket = connect(socketName);
                    }
                    if (audio) {
                        audioLocalSocket = connect(socketName);
                    }
                    if (control) {
                        controlLocalSocket = connect(socketName);
                    }
                }
            }
        } catch (IOException | RuntimeException e) {
            if (videoLocalSocket != null) {
                videoLocalSocket.close();
            }
            if (audioLocalSocket != null) {
                audioLocalSocket.close();
            }
            if (controlLocalSocket != null) {
                controlLocalSocket.close();
            }
            if (videoSocket != null) {
                videoSocket.close();
            }
            if (audioSocket != null) {
                audioSocket.close();
            }
            if (controlSocket != null) {
                controlSocket.close();
            }
            throw e;
        }

        return new DesktopConnection(videoLocalSocket, audioLocalSocket, controlLocalSocket,
                videoSocket, audioSocket, controlSocket);
    }



    public void shutdown() throws IOException {
        if (localVideoSocket != null) {
            localVideoSocket.shutdownInput();
            localVideoSocket.shutdownOutput();
        } else if (videoSocket != null) {
            videoSocket.shutdownInput();
            videoSocket.shutdownOutput();
        }
        if (localAudioSocket != null) {
            localAudioSocket.shutdownInput();
            localAudioSocket.shutdownOutput();
        } else if (audioSocket != null) {
            audioSocket.shutdownInput();
            audioSocket.shutdownOutput();
        }
        if (localControlSocket != null) {
            localControlSocket.shutdownInput();
            localControlSocket.shutdownOutput();
        } else if (controlSocket != null) {
            controlSocket.shutdownInput();
            controlSocket.shutdownOutput();
        }
    }

    public void close() throws IOException {
        if (localVideoSocket != null) {
            localVideoSocket.close();
        } else if (videoSocket != null) {
            videoSocket.close();
        }
        if (localAudioSocket != null) {
            localAudioSocket.close();
        } else if (audioSocket != null) {
            audioSocket.close();
        }
        if (localControlSocket != null) {
            localControlSocket.close();
        } else if (controlSocket != null) {
            controlSocket.close();
        }
    }

    public void sendDeviceMeta(String deviceName) throws IOException {
        byte[] buffer = new byte[DEVICE_NAME_FIELD_LENGTH];

        byte[] deviceNameBytes = deviceName.getBytes(StandardCharsets.UTF_8);
        int len = StringUtils.getUtf8TruncationIndex(deviceNameBytes, DEVICE_NAME_FIELD_LENGTH - 1);
        System.arraycopy(deviceNameBytes, 0, buffer, 0, len);
        // byte[] are always 0-initialized in java, no need to set '\0' explicitly

        if (localVideoSocket != null) {
            IO.writeFully(localVideoSocket.getFileDescriptor(), buffer, 0, buffer.length);
        } else if (videoSocket != null) {
            videoSocket.getOutputStream().write(buffer);
        } else if (localAudioSocket != null) {
            IO.writeFully(localAudioSocket.getFileDescriptor(), buffer, 0, buffer.length);
        } else if (audioSocket != null) {
            audioSocket.getOutputStream().write(buffer);
        } else if (localControlSocket != null) {
            IO.writeFully(localControlSocket.getFileDescriptor(), buffer, 0, buffer.length);
        } else if (controlSocket != null) {
            controlSocket.getOutputStream().write(buffer);
        }
    }

    public FileDescriptor getVideoFd() {
        return videoFd;
    }

    public InputStream getVideoInputStream() throws IOException {
        if (localVideoSocket != null) {
            return localVideoSocket.getInputStream();
        } else if (videoSocket != null) {
            return videoSocket.getInputStream();
        }
        return null;
    }

    public OutputStream getVideoOutputStream() throws IOException {
        if (localVideoSocket != null) {
            return localVideoSocket.getOutputStream();
        } else if (videoSocket != null) {
            return videoSocket.getOutputStream();
        }
        return null;
    }

    public FileDescriptor getAudioFd() {
        return audioFd;
    }

    public InputStream getAudioInputStream() throws IOException {
        if (localAudioSocket != null) {
            return localAudioSocket.getInputStream();
        } else if (audioSocket != null) {
            return audioSocket.getInputStream();
        }
        return null;
    }

    public OutputStream getAudioOutputStream() throws IOException {
        if (localAudioSocket != null) {
            return localAudioSocket.getOutputStream();
        } else if (audioSocket != null) {
            return audioSocket.getOutputStream();
        }
        return null;
    }

    public ControlChannel getControlChannel() {
        return controlChannel;
    }
}
