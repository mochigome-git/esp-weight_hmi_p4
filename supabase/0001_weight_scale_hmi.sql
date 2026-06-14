-- =============================================================================
-- Weighing scale HMI - Supabase schema
-- File: supabase/migrations/0001_weight_scale_hmi.sql
-- =============================================================================
-- This expects an existing `tenants` table with column `id uuid primary key`.
-- Adjust schema name if your project uses something other than `app`.
-- =============================================================================

-- ----------------------------------------------------------------------------
-- weight_models: catalog of named target specs (lower, standard, upper).
-- Pushed to devices via MQTT by the Go service after any change here.
-- ----------------------------------------------------------------------------
create table if not exists app.weight_models (
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
    on app.weight_models (tenant_id, name)
    where deleted_at is null;

create index if not exists weight_models_tenant_updated
    on app.weight_models (tenant_id, updated_at desc);

-- ----------------------------------------------------------------------------
-- weight_devices: track which devices have been seen, their online state.
-- Updated by Go svc from MQTT status topic (retained + LWT).
-- ----------------------------------------------------------------------------
create table if not exists app.weight_devices (
    device_id           text primary key,
    tenant_id           uuid not null references public.tenants(id) on delete cascade,
    online              boolean not null default false,
    mode                text check (mode in ('idle', 'run')),
    current_model_id    uuid references app.weight_models(id) on delete set null,
    last_seen_at        timestamptz not null default now(),
    first_seen_at       timestamptz not null default now()
);

create index if not exists weight_devices_tenant
    on app.weight_devices (tenant_id);

-- ----------------------------------------------------------------------------
-- updated_at trigger
-- ----------------------------------------------------------------------------
create or replace function app.weight_models_touch_updated_at()
returns trigger
language plpgsql
as $$
begin
    new.updated_at = now();
    return new;
end;
$$;

drop trigger if exists weight_models_set_updated_at on app.weight_models;
create trigger weight_models_set_updated_at
    before update on app.weight_models
    for each row execute function app.weight_models_touch_updated_at();

-- ----------------------------------------------------------------------------
-- RLS
-- ----------------------------------------------------------------------------
alter table app.weight_models    enable row level security;
alter table app.weight_readings  enable row level security;
alter table app.weight_devices   enable row level security;

-- Adjust this policy to match your tenant access pattern. Below assumes
-- `auth.jwt()` carries a `tenant_id` claim. Modify as needed for your auth.
create policy weight_models_tenant_isolation on app.weight_models
    for all
    using  (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid)
    with check (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid);

create policy weight_readings_tenant_isolation on app.weight_readings
    for all
    using  (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid)
    with check (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid);

create policy weight_devices_tenant_isolation on app.weight_devices
    for all
    using  (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid)
    with check (tenant_id = (auth.jwt() ->> 'tenant_id')::uuid);
