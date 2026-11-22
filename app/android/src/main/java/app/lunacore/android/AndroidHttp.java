// SPDX-License-Identifier: GPL-3.0-only

package app.lunacore.android;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import javax.net.ssl.HttpsURLConnection;

final class AndroidHttp {
    private static final int MAX_RESPONSE = 32 * 1024 * 1024;

    private static URL https(String text) throws IOException {
        URL url = new URL(text);
        if (!"https".equals(url.getProtocol()) || url.getHost().isEmpty() || url.getUserInfo() != null)
            throw new IOException("Invalid HTTPS URL");
        return url;
    }

    static byte[][] request(String method, String address, String agent, String[] headers,
                            byte[] body, int connectMs, int totalMs) throws IOException {
        long deadline = System.nanoTime() + totalMs * 1_000_000L;
        URL origin = https(address);
        URL url = origin;
        for (int redirect = 0; redirect <= 3; redirect++) {
            HttpsURLConnection connection = (HttpsURLConnection) url.openConnection();
            try {
                connection.setInstanceFollowRedirects(false);
                connection.setConnectTimeout(Math.min(connectMs, totalMs));
                connection.setReadTimeout(totalMs);
                connection.setUseCaches(false);
                connection.setRequestMethod(method);
                connection.setRequestProperty("User-Agent", agent);
                for (String header : headers) {
                    int colon = header.indexOf(':');
                    if (colon > 0)
                        connection.setRequestProperty(header.substring(0, colon), header.substring(colon + 1).trim());
                }
                if (body.length != 0) {
                    connection.setDoOutput(true);
                    connection.setFixedLengthStreamingMode(body.length);
                    try (OutputStream stream = connection.getOutputStream()) {
                        stream.write(body);
                    }
                }
                int status = connection.getResponseCode();
                String location = connection.getHeaderField("Location");
                if (location != null && status >= 300 && status <= 308 && status != 304) {
                    URL next = https(new URL(url, location).toExternalForm());
                    if (origin.getHost().equalsIgnoreCase(next.getHost()) && port(origin) == port(next)
                            && ("GET".equals(method) || status == 307 || status == 308)) {
                        if (System.nanoTime() > deadline) throw new IOException("HTTPS deadline");
                        url = next;
                        continue;
                    }
                }
                String errorType = connection.getHeaderField("x-amzn-ErrorType");
                InputStream bodyStream = status >= 400 ? connection.getErrorStream() : connection.getInputStream();
                byte[] bytes = new byte[0];
                if (bodyStream != null) {
                    try (InputStream stream = bodyStream; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                        byte[] block = new byte[8192];
                        int count;
                        while ((count = stream.read(block)) >= 0) {
                            if (count > MAX_RESPONSE - output.size() || System.nanoTime() > deadline)
                                throw new IOException("HTTPS response limit");
                            output.write(block, 0, count);
                        }
                        bytes = output.toByteArray();
                    }
                }
                return new byte[][] {
                    Integer.toString(status).getBytes(StandardCharsets.US_ASCII), bytes,
                    errorType == null ? new byte[0] : errorType.getBytes(StandardCharsets.UTF_8)
                };
            } finally {
                connection.disconnect();
            }
        }
        throw new IOException("HTTPS redirect limit");
    }

    static int measureTcp(String address, int timeoutMs) throws IOException {
        URL url = https(address);
        InetSocketAddress endpoint = new InetSocketAddress(url.getHost(), port(url));
        long start = System.nanoTime();
        try (Socket socket = new Socket()) {
            socket.connect(endpoint, timeoutMs);
            return (int) ((System.nanoTime() - start) / 1_000_000L);
        }
    }

    private static int port(URL url) {
        return url.getPort() < 0 ? 443 : url.getPort();
    }
}
