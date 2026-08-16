param(
    [string]$LocalAddress = "192.168.1.6",
    [string]$RemoteAddress = "192.168.1.10",
    [uint16]$LocalPort = 1883
)

$ErrorActionPreference = "Stop"

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "请以管理员身份运行此脚本"
}

$ruleName = "EmbeddedEdgeGateway-MQTT-WSL"
$displayName = "Embedded Edge Gateway MQTT (Raspberry Pi to WSL)"
$wslCreatorId = "{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}"
$existing = Get-NetFirewallHyperVRule -Name $ruleName -ErrorAction SilentlyContinue

$parameters = @{
    Direction       = "Inbound"
    VMCreatorId     = $wslCreatorId
    Protocol        = "TCP"
    LocalAddresses  = @($LocalAddress)
    LocalPorts      = @([string]$LocalPort)
    RemoteAddresses = @($RemoteAddress)
    Action          = "Allow"
    Enabled         = "True"
}

if ($null -eq $existing) {
    New-NetFirewallHyperVRule -Name $ruleName -DisplayName $displayName @parameters | Out-Null
    Write-Host "Created Hyper-V firewall rule: $ruleName"
} else {
    Set-NetFirewallHyperVRule -Name $ruleName -NewDisplayName $displayName @parameters | Out-Null
    Write-Host "Updated Hyper-V firewall rule: $ruleName"
}

Get-NetFirewallHyperVRule -Name $ruleName |
    Select-Object Name, Enabled, Direction, Action, Protocol, LocalAddresses, LocalPorts, RemoteAddresses |
    Format-List
