-- =============================================================================
-- Weighing scale HMI - Supabase schema
-- File: supabase/migrations/0001_weight_scale_hmi.sql
-- =============================================================================
-- This expects an existing `tenants` table with column `id uuid primary key`.
-- Adjust schema name if your project uses something other than `public`.
-- =============================================================================

-- ----------------------------------------------------------------------------
-- weight_models: catalog of named target specs (lower, standard, upper).
-- Pushed to devices via MQTT by the Go service after any change here.
-- ----------------------------------------------------------------------------
create table if not exists public.weight_models (
    id           uuid primary key default gen_random_uuid(),
    tenant_id    uuid not null references public.tenants(id) on delete cascade,
    name         text not null,
    lower_limit  numeric(12, 4) not null,
    standard     numeric(12, 4) not null,
    upper_limit  numeric(12, 4) not null,
    unit         text not null default 'g'
                  check (unit in ('g', 'kg', 'lb', 'oz')),
    created_at   timestamptz not null default now(),
    updated_at   timestamptz not null default now(),
    deleted_at   timestamptz,
    constraint   weight_models_limits_order check (
                     lower_limit <= standard and standard <= upper_limit
                 )
);

create unique index if not exists weight_models_tenant_name_unique
    on public.weight_models (tenant_id, name)
    where deleted_at is null;

create index if not exists weight_models_tenant_updated
    on public.weight_models (tenant_id, updated_at desc);

-- ----------------------------------------------------------------------------
-- weight_readings: every stable reading published by a device.
-- One row per stable-event MQTT publish.
-- ----------------------------------------------------------------------------
create table if not exists public.weight_readings (
    id            uuid primary key default gen_random_uuid(),
    tenant_id     uuid not null references public.tenants(id) on delete cascade,
    device_id     text not null,    -- format: 'p4-xxxxxxxxxxxx' from MAC
    model_id      uuid references public.weight_models(id) on delete set null,
    model_name    text,             -- snapshotted for resilience to model rename/delete
    reading       numeric(12, 4) not null,
    unit          text not null,
    status        text not null check (status in ('pass', 'high', 'low')),
    lower_limit   numeric(12, 4),   -- snapshotted limits at publish time
    standard      numeric(12, 4),
    upper_limit   numeric(12, 4),
    recorded_at   timestamptz not null,
    received_at   timestamptz not null default now()
);

create index if not exists weight_readings_tenant_device_time
    on public.weight_readings (tenant_id, device_id, recorded_at desc);

create index if not exists weight_readings_tenant_model_time
    on public.weight_readings (tenant_id, model_id, recorded_at desc);

-- ----------------------------------------------------------------------------
-- weight_devices: track which devices have been seen, their online state.
-- Updated by Go svc from MQTT status topic (retained + LWT).
-- ----------------------------------------------------------------------------
create table if not exists public.weight_devices (
    device_id           text primary key,
    tenant_id           uuid not null references public.tenants(id) on delete cascade,
    online              boolean not null default false,
    mode                text check (mode in ('idle', 'run')),
    current_model_id    uuid references public.weight_models(id) on delete set null,
    last_seen_at        timestamptz not null default now(),
    first_seen_at       timestamptz not null default now()
);

create index if not exists weight_devices_tenant
    on public.weight_devices (tenant_id);

-- ----------------------------------------------------------------------------
-- updated_at trigger
-- ----------------------------------------------------------------------------
create or replace function public.weight_models_touch_updated_at()
returns trigger
language plpgsql
as $$
begin
    new.updated_at = now();
    return new;
end;
$$;

drop trigger if exists weight_models_set_updated_at on public.weight_models;
create trigger weight_models_set_updated_at
    before update on public.weight_models
    for each row execute function public.weight_models_touch_updated_at();

-- ----------------------------------------------------------------------------
-- RLS
-- ----------------------------------------------------------------------------
alter table public.weight_models    enable row level security;
alter table public.weight_readings  enable row level security;
alter table public.weight_devices   enable row level security;

-- Adjust this policy to match your tenant access pattern. Below assumes
-- `auth.jwt()` carries a `tenant_id` claim. Modify as needed for your auth.
create policy weight_models_tenant_isolation on public.weight_models
    for all
    using  (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid)
    with check (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid);

create policy weight_readings_tenant_isolation on public.weight_readings
    for all
    using  (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid)
    with check (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid);

create policy weight_devices_tenant_isolation on public.weight_devices
    for all
    using  (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid)
    with check (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid);
