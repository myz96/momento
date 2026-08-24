# Momento Shared Packages

This directory contains cross-app contracts, protocols, and shared types.

## Purpose

- **Protocol definitions**: Firmware ↔ Backend communication
- **OpenAPI specs**: Backend API contracts for Mobile app
- **Protobuf schemas**: If using gRPC/protocol buffers
- **Shared TypeScript types**: Mobile app type definitions generated from API specs
- **Message formats**: BLE/Wi-Fi protocol definitions for firmware-mobile communication

## Structure (when populated)

```
packages/
├── api-contracts/        # OpenAPI spec, generated clients
├── proto/                 # Protocol buffer definitions (if used)
├── ble-protocol/          # BLE service/characteristic definitions
└── typescript-types/      # Shared TS types for mobile app
```

## Usage

Contracts defined here should be:
1. Version-controlled (schemas are source of truth)
2. Auto-generated code (clients, types) committed to each sub-app's `generated/` directory
3. Updated via CI/CD on schema changes

## Status

Empty placeholder. Add contracts as cross-app communication requirements emerge.
