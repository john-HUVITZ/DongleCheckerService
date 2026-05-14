using System;
using System.ComponentModel;
using System.Net.Http;
using System.Net.Sockets;
using System.ServiceProcess;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;

namespace DongleCheckTest
{
    public partial class MainWindow : Window
    {
        private static readonly HttpClient Client = new()
        {
            Timeout = TimeSpan.FromSeconds(10)
        };

        private static readonly string[] DongleCheckerServiceNames =
        {
            "DongleCheckerSvc",
            "DongleChecker"
        };

        public MainWindow()
        {
            InitializeComponent();
        }

        private async void RequestButton_Click(object sender, RoutedEventArgs e)
        {
            ClearResult();

            if (!TryCreateLicenseUri(out var uri, out var validationMessage))
            {
                SetStatus(validationMessage, Brushes.Firebrick);
                return;
            }

            RequestButton.IsEnabled = false;
            SetStatus($"Requesting {uri} ...", Brushes.DimGray);

            try
            {
                using var response = await Client.GetAsync(uri);
                var body = await response.Content.ReadAsStringAsync();

                RawResponseTextBox.Text = FormatJson(body);

                if (response.IsSuccessStatusCode)
                {
                    DisplayLicenseResult(body);
                    SetStatus($"Success: HTTP {(int)response.StatusCode} {response.ReasonPhrase}", Brushes.ForestGreen);
                }
                else
                {
                    SetStatus($"Failed: HTTP {(int)response.StatusCode} {response.ReasonPhrase}", Brushes.Firebrick);
                }
            }
            catch (TaskCanceledException)
            {
                SetStatus("Request timed out.", Brushes.Firebrick);
            }
            catch (HttpRequestException ex)
            {
                SetStatus($"Request failed: {ex.Message}", Brushes.Firebrick);
            }
            catch (Exception ex)
            {
                SetStatus($"Unexpected error: {ex.Message}", Brushes.Firebrick);
            }
            finally
            {
                RequestButton.IsEnabled = true;
            }
        }

        private async void StopServiceButton_Click(object sender, RoutedEventArgs e)
        {
            StopServiceButton.IsEnabled = false;
            SetStatus("Stopping DongleCheckerSvc service ...", Brushes.DimGray);

            try
            {
                var result = await Task.Run(StopDongleCheckerService);
                SetStatus(result, Brushes.ForestGreen);
            }
            catch (InvalidOperationException ex)
            {
                SetStatus(ex.Message, Brushes.Firebrick);
            }
            catch (Win32Exception ex)
            {
                SetStatus($"서비스 제어 권한이 없거나 실패했습니다. 관리자 권한으로 실행하세요. ({ex.Message})", Brushes.Firebrick);
            }
            catch (Exception ex)
            {
                SetStatus($"서비스 종료 실패: {ex.Message}", Brushes.Firebrick);
            }
            finally
            {
                StopServiceButton.IsEnabled = true;
            }
        }

        private static string StopDongleCheckerService()
        {
            using var service = OpenDongleCheckerService();

            if (service.Status == ServiceControllerStatus.Stopped)
            {
                return $"{service.ServiceName} 서비스는 이미 중지되어 있습니다.";
            }

            if (!service.CanStop)
            {
                throw new InvalidOperationException($"{service.ServiceName} 서비스는 현재 중지할 수 없는 상태입니다. Status={service.Status}");
            }

            service.Stop();
            service.WaitForStatus(ServiceControllerStatus.Stopped, TimeSpan.FromSeconds(15));
            return $"{service.ServiceName} 서비스가 중지되었습니다.";
        }

        private static ServiceController OpenDongleCheckerService()
        {
            foreach (var serviceName in DongleCheckerServiceNames)
            {
                var service = new ServiceController(serviceName);
                try
                {
                    _ = service.Status;
                    return service;
                }
                catch (InvalidOperationException)
                {
                    service.Dispose();
                }
            }

            throw new InvalidOperationException("DongleCheckerSvc 서비스를 찾을 수 없습니다.");
        }

        private bool TryCreateLicenseUri(out Uri uri, out string message)
        {
            uri = null!;
            message = string.Empty;

            var host = IpTextBox.Text.Trim();
            if (string.IsNullOrWhiteSpace(host))
            {
                message = "IP를 입력하세요.";
                return false;
            }

            if (!int.TryParse(PortTextBox.Text.Trim(), out var port) || port < 1 || port > 65535)
            {
                message = "Port는 1부터 65535 사이 숫자여야 합니다.";
                return false;
            }

            var builder = new UriBuilder("http", NormalizeHost(host), port, "license");
            uri = builder.Uri;
            return true;
        }

        private static string NormalizeHost(string host)
        {
            if (Uri.TryCreate(host, UriKind.Absolute, out var absoluteUri) && !string.IsNullOrWhiteSpace(absoluteUri.Host))
            {
                host = absoluteUri.Host;
            }

            if (System.Net.IPAddress.TryParse(host, out var address) &&
                address.AddressFamily == AddressFamily.InterNetworkV6)
            {
                return $"[{host.Trim('[', ']')}]";
            }

            return host;
        }

        private void DisplayLicenseResult(string body)
        {
            try
            {
                var result = JsonSerializer.Deserialize<LicenseResult>(
                    body,
                    new JsonSerializerOptions { PropertyNameCaseInsensitive = true });

                if (result == null)
                {
                    SetStatus("Response is empty.", Brushes.Firebrick);
                    return;
                }

                IsConnTextBlock.Text = result.IsConn?.ToString() ?? "-";
                KeyTextBlock.Text = result.Key?.ToString() ?? "-";
                SerialNumberTextBlock.Text = result.SerialNumber ?? "-";
                DateTextBlock.Text = result.Date ?? "-";
            }
            catch (JsonException)
            {
                SetStatus("Response is not valid JSON. Check Raw Response.", Brushes.DarkOrange);
            }
        }

        private static string FormatJson(string body)
        {
            if (string.IsNullOrWhiteSpace(body))
            {
                return string.Empty;
            }

            try
            {
                using var document = JsonDocument.Parse(body);
                return JsonSerializer.Serialize(
                    document.RootElement,
                    new JsonSerializerOptions { WriteIndented = true });
            }
            catch (JsonException)
            {
                return body;
            }
        }

        private void ClearResult()
        {
            IsConnTextBlock.Text = "-";
            KeyTextBlock.Text = "-";
            SerialNumberTextBlock.Text = "-";
            DateTextBlock.Text = "-";
            RawResponseTextBox.Text = string.Empty;
        }

        private void SetStatus(string message, Brush foreground)
        {
            StatusTextBlock.Text = message;
            StatusTextBlock.Foreground = foreground;
        }

        private sealed class LicenseResult
        {
            public bool? IsConn { get; set; }

            public int? Key { get; set; }

            public string? SerialNumber { get; set; }

            public string? Date { get; set; }
        }
    }
}
