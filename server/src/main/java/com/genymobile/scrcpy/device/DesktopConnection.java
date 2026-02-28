package com.genymobile.scrcpy.device;

import com.genymobile.scrcpy.control.ControlChannel;
import com.genymobile.scrcpy.util.Ln;
import com.genymobile.scrcpy.util.StringUtils;

import java.io.Closeable;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.charset.StandardCharsets;

public final class DesktopConnection implements Closeable {

    private static final int DEVICE_NAME_FIELD_LENGTH = 64;

    private final Socket videoSocket;
    private final Socket audioSocket;
    private final Socket controlSocket;

    private final ControlChannel controlChannel;

    private DesktopConnection(Socket videoSocket, Socket audioSocket, Socket controlSocket) throws IOException {
        this.videoSocket = videoSocket;
        this.audioSocket = audioSocket;
        this.controlSocket = controlSocket;

        if (controlSocket != null) {
            controlChannel = new ControlChannel(controlSocket);
        } else {
            controlChannel = null;
        }
    }

    public static DesktopConnection open(int scid, boolean tunnelForward, boolean video, boolean audio, boolean control, boolean sendDummyByte,
            String serverHost, int connectVideoPort, int connectAudioPort, int connectControlPort) throws IOException {
        Socket videoSocket = null;
        Socket audioSocket = null;
        Socket controlSocket = null;
        String channel = "none";
        int port = -1;

        // Validate parameters
        if (serverHost == null || serverHost.isEmpty()) {
            throw new IllegalArgumentException("serverHost cannot be null or empty");
        }
        if (connectVideoPort < 1 || connectVideoPort > 65535) {
            throw new IllegalArgumentException("Video port must be between 1 and 65535");
        }
        if (connectAudioPort < 1 || connectAudioPort > 65535) {
            throw new IllegalArgumentException("Audio port must be between 1 and 65535");
        }
        if (connectControlPort < 1 || connectControlPort > 65535) {
            throw new IllegalArgumentException("Control port must be between 1 and 65535");
        }

        try {
            // Client mode - device connects to PC server
            if (video) {
                channel = "video";
                port = connectVideoPort;
                Ln.d("connecting video host=" + serverHost + " port=" + connectVideoPort
                        + " enabled(video/audio/control)=" + video + "/" + audio + "/" + control);
                videoSocket = new Socket();
                videoSocket.connect(new java.net.InetSocketAddress(serverHost, connectVideoPort));
                if (sendDummyByte) {
                    videoSocket.getOutputStream().write(0);
                    sendDummyByte = false;
                }
            }
            if (audio) {
                channel = "audio";
                port = connectAudioPort;
                Ln.d("connecting audio host=" + serverHost + " port=" + connectAudioPort
                        + " enabled(video/audio/control)=" + video + "/" + audio + "/" + control);
                audioSocket = new Socket();
                audioSocket.connect(new java.net.InetSocketAddress(serverHost, connectAudioPort));
                if (sendDummyByte) {
                    audioSocket.getOutputStream().write(0);
                    sendDummyByte = false;
                }
            }
            if (control) {
                channel = "control";
                port = connectControlPort;
                Ln.d("connecting control host=" + serverHost + " port=" + connectControlPort
                        + " enabled(video/audio/control)=" + video + "/" + audio + "/" + control);
                controlSocket = new Socket();
                controlSocket.connect(new java.net.InetSocketAddress(serverHost, connectControlPort));
                if (sendDummyByte) {
                    controlSocket.getOutputStream().write(0);
                    sendDummyByte = false;
                }
            }
        } catch (IOException | RuntimeException e) {
            Ln.e("Connection failed channel=" + channel + " host=" + serverHost + " port=" + port
                    + " enabled(video/audio/control)=" + video + "/" + audio + "/" + control, e);
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

        return new DesktopConnection(videoSocket, audioSocket, controlSocket);
    }

    public void shutdown() throws IOException {
        if (videoSocket != null) {
            try {
                videoSocket.shutdownInput();
                videoSocket.shutdownOutput();
            } catch (IOException e) {
                // ignore (socket may already be disconnected)
            }
        }
        if (audioSocket != null) {
            try {
                audioSocket.shutdownInput();
                audioSocket.shutdownOutput();
            } catch (IOException e) {
                // ignore (socket may already be disconnected)
            }
        }
        if (controlSocket != null) {
            try {
                controlSocket.shutdownInput();
                controlSocket.shutdownOutput();
            } catch (IOException e) {
                // ignore (socket may already be disconnected)
            }
        }
    }

    public void close() throws IOException {
        if (videoSocket != null) {
            videoSocket.close();
        }
        if (audioSocket != null) {
            audioSocket.close();
        }
        if (controlSocket != null) {
            controlSocket.close();
        }
    }

    public void sendDeviceMeta(String deviceName) throws IOException {
        byte[] buffer = new byte[DEVICE_NAME_FIELD_LENGTH];

        byte[] deviceNameBytes = deviceName.getBytes(StandardCharsets.UTF_8);
        int len = StringUtils.getUtf8TruncationIndex(deviceNameBytes, DEVICE_NAME_FIELD_LENGTH - 1);
        System.arraycopy(deviceNameBytes, 0, buffer, 0, len);

        // Send device meta to the video socket (primary stream)
        if (videoSocket != null) {
            videoSocket.getOutputStream().write(buffer);
        } else if (audioSocket != null) {
            audioSocket.getOutputStream().write(buffer);
        } else if (controlSocket != null) {
            controlSocket.getOutputStream().write(buffer);
        }
    }

    public InputStream getVideoInputStream() throws IOException {
        return videoSocket != null ? videoSocket.getInputStream() : null;
    }

    public OutputStream getVideoOutputStream() throws IOException {
        return videoSocket != null ? videoSocket.getOutputStream() : null;
    }

    public InputStream getAudioInputStream() throws IOException {
        return audioSocket != null ? audioSocket.getInputStream() : null;
    }

    public OutputStream getAudioOutputStream() throws IOException {
        return audioSocket != null ? audioSocket.getOutputStream() : null;
    }

    public ControlChannel getControlChannel() {
        return controlChannel;
    }
}
