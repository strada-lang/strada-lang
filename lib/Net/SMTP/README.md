# Net::SMTP - SMTP Client Library for Strada

A pure Strada implementation of an SMTP client for sending emails.

## Features

- Plain SMTP connections (port 25, 587)
- AUTH LOGIN and AUTH PLAIN authentication
- Multiple recipients support
- Automatic dot-stuffing for message body
- Debug mode to see SMTP conversation
- Convenience method for sending complete emails
- Uses system `sys::base64_encode()` for authentication

## Installation

The library is part of the Strada standard library. No additional installation required.

## Quick Start

```strada
use lib "lib";
use Net::SMTP;

func main() int {
    # Connect to SMTP server
    my scalar $smtp = SMTP::new("smtp.example.com", 25);
    if (!defined($smtp)) {
        die("Failed to connect");
    }

    # Greet the server
    $smtp->ehlo("mydomain.com");

    # Authenticate (if required)
    $smtp->auth("username", "password");

    # Send email
    $smtp->mail("sender@example.com");
    $smtp->rcpt_to("recipient@example.com");
    $smtp->data("Subject: Test\r\n\r\nHello, World!");

    # Disconnect
    $smtp->quit();

    return 0;
}
```

## API Reference

### Constructors

#### `SMTP::new($host, $port)` -> scalar
Connect to an SMTP server using plain TCP.

```strada
my scalar $smtp = SMTP::new("localhost", 25);
my scalar $smtp = SMTP::new("smtp.example.com", 587);
```

Returns a blessed SMTP object or `undef` on connection failure.

### Methods

#### `$smtp->debug($on)` -> void
Enable (1) or disable (0) debug mode. When enabled, prints all SMTP commands and responses.

```strada
$smtp->debug(1);  # Enable
$smtp->debug(0);  # Disable
```

#### `$smtp->helo($domain)` -> int
Send HELO greeting. Returns 1 on success, 0 on failure.

```strada
$smtp->helo("mydomain.com");
```

#### `$smtp->ehlo($domain)` -> int
Send EHLO greeting (extended SMTP). Returns 1 on success, 0 on failure. Use this to discover server capabilities.

```strada
if ($smtp->ehlo("mydomain.com")) {
    say("Capabilities: " . $smtp->message());
}
```

#### `$smtp->starttls()` -> int
Send STARTTLS command to upgrade connection to TLS. Returns 1 on success.

Note: After STARTTLS, you need to perform the TLS handshake. This is typically handled by reconnecting with SSL.

#### `$smtp->auth($username, $password)` -> int
Authenticate using AUTH LOGIN mechanism. Returns 1 on success.

```strada
if (!$smtp->auth("user@example.com", "mypassword")) {
    die("Authentication failed: " . $smtp->message());
}
```

#### `$smtp->auth_plain($username, $password)` -> int
Authenticate using AUTH PLAIN mechanism. Returns 1 on success.

```strada
$smtp->auth_plain("user@example.com", "mypassword");
```

#### `$smtp->mail($from)` -> int
Specify the sender address (MAIL FROM command). Returns 1 on success.

```strada
$smtp->mail("sender@example.com");
```

#### `$smtp->rcpt_to($recipient)` -> int
Specify a recipient address (RCPT TO command). Call multiple times for multiple recipients. Returns 1 on success.

```strada
$smtp->rcpt_to("recipient1@example.com");
$smtp->rcpt_to("recipient2@example.com");
```

#### `$smtp->recipient($rcpt)` -> int
Alias for `$smtp->rcpt_to()`.

#### `$smtp->data($message)` -> int
Send the email message (DATA command). The message should include headers. Returns 1 on success.

```strada
my str $message = "From: sender@example.com\r\n";
$message = $message . "To: recipient@example.com\r\n";
$message = $message . "Subject: Test Email\r\n";
$message = $message . "\r\n";
$message = $message . "This is the email body.";

$smtp->data($message);
```

#### `$smtp->reset()` -> int
Reset the mail transaction (RSET command). Returns 1 on success.

#### `$smtp->noop()` -> int
Send NOOP command (keep-alive). Returns 1 on success.

#### `$smtp->verify($address)` -> int
Verify an email address (VRFY command). Often disabled on servers.

#### `$smtp->quit()` -> int
Close the connection (QUIT command). Returns 1 on success.

```strada
$smtp->quit();
```

#### `$smtp->code()` -> int
Get the last SMTP response code.

```strada
if ($smtp->code() != 250) {
    say("Error code: " . $smtp->code());
}
```

#### `$smtp->message()` -> str
Get the last SMTP response message.

```strada
say("Server said: " . $smtp->message());
```

### Convenience Methods

#### `$smtp->send_mail($from, $to_list, $subject, $body)` -> int
Send a complete email with headers automatically generated.

```strada
my array @recipients = ("user1@example.com", "user2@example.com");

if ($smtp->send_mail("sender@example.com", \@recipients, "Subject", "Body text")) {
    say("Email sent!");
}
```

## Examples

### Send Email Without Authentication

```strada
use lib "lib";
use Net::SMTP;

func main() int {
    my scalar $smtp = SMTP::new("localhost", 25);
    if (!defined($smtp)) {
        die("Connection failed");
    }

    $smtp->ehlo("localhost");

    my array @to = ("recipient@example.com");
    $smtp->send_mail("sender@example.com", \@to, "Test Subject", "Hello!\n\nThis is a test.");

    $smtp->quit();
    return 0;
}
```

### Send Email With Authentication

```strada
use lib "lib";
use Net::SMTP;

func main() int {
    my scalar $smtp = SMTP::new("smtp.example.com", 587);
    if (!defined($smtp)) {
        die("Connection failed");
    }

    $smtp->debug(1);  # See SMTP conversation

    $smtp->ehlo("mydomain.com");

    if (!$smtp->auth("myuser", "mypassword")) {
        die("Authentication failed");
    }

    my array @to = ("recipient@example.com");
    $smtp->send_mail("myuser@example.com", \@to, "Hello", "This is a test email.");

    $smtp->quit();
    return 0;
}
```

### Manual Message Construction

```strada
use lib "lib";
use Net::SMTP;

func main() int {
    my scalar $smtp = SMTP::new("localhost", 25);
    $smtp->ehlo("localhost");

    $smtp->mail("sender@example.com");
    $smtp->rcpt_to("recipient1@example.com");
    $smtp->rcpt_to("recipient2@example.com");

    # Manually construct message with custom headers
    my str $msg = "From: Sender Name <sender@example.com>\r\n";
    $msg = $msg . "To: recipient1@example.com, recipient2@example.com\r\n";
    $msg = $msg . "Subject: Custom Headers Example\r\n";
    $msg = $msg . "X-Mailer: Strada Net::SMTP\r\n";
    $msg = $msg . "Content-Type: text/plain; charset=UTF-8\r\n";
    $msg = $msg . "\r\n";
    $msg = $msg . "This email has custom headers.\r\n";
    $msg = $msg . "\r\n";
    $msg = $msg . "Best regards,\r\n";
    $msg = $msg . "Strada";

    $smtp->data($msg);
    $smtp->quit();

    return 0;
}
```

## Running the Example

Build and run the included example:

```bash
cd /path/to/strada
./strada lib/Net/SMTP/example.strada

# Test connection
./example test localhost 25

# Send without auth (local mail server)
./example send localhost 25 from@local to@local "Test" "Hello World"

# Send with authentication
./example send-auth smtp.example.com 587 user pass from@example.com to@example.com "Subject" "Body"
```

## SMTP Response Codes

| Code | Meaning |
|------|---------|
| 220 | Service ready |
| 221 | Service closing |
| 235 | Authentication successful |
| 250 | Requested action OK |
| 251 | User not local; will forward |
| 334 | Server challenge (AUTH) |
| 354 | Start mail input |
| 421 | Service not available |
| 450 | Mailbox unavailable |
| 451 | Local error |
| 452 | Insufficient storage |
| 500 | Syntax error |
| 501 | Syntax error in parameters |
| 502 | Command not implemented |
| 503 | Bad sequence of commands |
| 504 | Parameter not implemented |
| 530 | Authentication required |
| 535 | Authentication failed |
| 550 | Mailbox unavailable |
| 551 | User not local |
| 552 | Storage exceeded |
| 553 | Mailbox name not allowed |
| 554 | Transaction failed |

## Notes

- Port 25 is often blocked by ISPs; use 587 (submission) for authenticated email
- The library handles dot-stuffing automatically in the DATA command
- For SSL/TLS support, see the `lib/ssl` module

## See Also

- [RFC 5321](https://tools.ietf.org/html/rfc5321) - Simple Mail Transfer Protocol
- [RFC 4954](https://tools.ietf.org/html/rfc4954) - SMTP Authentication
- `lib/ssl/` - SSL/TLS library for secure connections
