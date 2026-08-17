# 为 WSL2 的 MQTT Broker 创建或更新精确限定来源的 Hyper-V 入站规则。
param(
    [string]$LocalAddress = "192.168.1.6",   # Windows/WSL 对局域网暴露的本地地址。
    [string]$RemoteAddress = "192.168.1.10", # 被允许连接 Broker 的树莓派地址。
    [uint16]$LocalPort = 1883                 # Mosquitto 的 TCP 监听端口。
)

$ErrorActionPreference = "Stop"  # 任一 cmdlet 失败都终止，避免留下部分配置。

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()          # 当前 Windows 登录身份。
$principal = New-Object Security.Principal.WindowsPrincipal($identity)  # 用于管理员角色判断的包装对象。
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "请以管理员身份运行此脚本"
}

$ruleName = "EmbeddedEdgeGateway-MQTT-WSL"  # 幂等查找和更新使用的稳定规则标识。
$displayName = "Embedded Edge Gateway MQTT (Raspberry Pi to WSL)"  # 控制台中的可读名称。
$wslCreatorId = "{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}"  # WSL2 使用的 Hyper-V VM Creator ID。
$existing = Get-NetFirewallHyperVRule -Name $ruleName -ErrorAction SilentlyContinue  # 已有规则或 $null。

# 新建和更新共用同一参数集合，确保重复执行不会产生规则漂移。
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
